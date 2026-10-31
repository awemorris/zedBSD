/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Inline SVG (ws074-p228): an <svg> element of an HTML page drawn into a
 * bitmap at its own size, which the layout then treats as a replaced
 * element's picture (as for an <img>).  The subset is what logos and icons
 * use: g, path (every command, arcs too), rect (rounded), circle, ellipse,
 * polygon and polyline filled; the viewBox with preserveAspectRatio's
 * default; transform lists; fill, fill-opacity, opacity, fill-rule and
 * display, from attributes and the style attribute, inherited through g; a
 * gradient fill by its first stop.  Strokes, clip paths, masks, filters,
 * <use>, text and CSS rules from style sheets are not drawn.
 *
 * Shapes are filled by scanlines with SVG_SUBLINES sub-scanlines per row and
 * exact horizontal coverage, nonzero or even-odd.
 */

#include "page/page.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The largest side of the bitmap, in pixels (a larger viewBox is scaled down). */
#define SVG_SIDE_MAX		512

/* The size of an svg without width, height and viewBox (CSS's default object size). */
#define SVG_DEFAULT_WIDTH	300.0f
#define SVG_DEFAULT_HEIGHT	150.0f

/* Sub-scanlines of one pixel row. */
#define SVG_SUBLINES		5

/* The deepest element nesting drawn. */
#define SVG_DEPTH		64

/* How many line segments a curve is cut into, and the most points of one shape. */
#define SVG_CURVE_STEPS		24
#define SVG_POINTS_MAX		200000

/* A fill rule. */
enum svg_rule {
	SVG_NONZERO,
	SVG_EVENODD
};

/* A 2D affine transform: x' = a*x + c*y + e, y' = b*x + d*y + f. */
struct svg_matrix {
	float a;
	float b;
	float c;
	float d;
	float e;
	float f;
};

/* The presentation state a shape inherits from its ancestors. */
struct svg_style {
	uint32_t fill;
	int fill_none;
	float fill_opacity;
	float opacity;
	enum svg_rule rule;
	uint32_t stroke;
	int stroke_none;
	float stroke_width;
	float stroke_opacity;
	int stroke_cap;
};

/* One point of a shape's outline; a point with begin set starts a new subpath. */
struct svg_point {
	float x;
	float y;
	int begin;
	int closed;
};

/* The outline being built: its points, in device space. */
struct svg_shape {
	struct svg_point *points;
	size_t count;
	size_t capacity;
};

/* The size of the viewport (the viewBox, else the bitmap) that percentage lengths of the picture being drawn are of. */
static float svg_view_width;
static float svg_view_height;

/* The bitmap being drawn: premultiplied float RGBA, and a row's coverage. */
struct svg_canvas {
	float *pixels;
	float *coverage;
	int width;
	int height;
};

/* One edge for the scanline: its upper end first, and its direction. */
struct svg_edge {
	float x0;
	float y0;
	float x1;
	float y1;
	int direction;
};

/* A named color. */
struct svg_color {
	const char *name;
	uint32_t value;
};

static const struct svg_color svg_colors[] = {
	{ "black", 0xff000000U }, { "white", 0xffffffffU }, { "red", 0xffff0000U },
	{ "green", 0xff008000U }, { "blue", 0xff0000ffU }, { "yellow", 0xffffff00U },
	{ "gray", 0xff808080U }, { "grey", 0xff808080U }, { "orange", 0xffffa500U },
	{ "purple", 0xff800080U }, { "silver", 0xffc0c0c0U }, { "lime", 0xff00ff00U },
	{ "navy", 0xff000080U }, { "teal", 0xff008080U }, { "maroon", 0xff800000U },
	{ "aqua", 0xff00ffffU }, { "cyan", 0xff00ffffU }, { "fuchsia", 0xffff00ffU },
	{ "magenta", 0xffff00ffU }, { "transparent", 0x00000000U }
};

static const char *svg_attribute(const struct dom_element *element, const char *name, struct wb_buffer *buffer);
static int svg_name_is(const struct dom_element *element, const char *name);
static int svg_same_name(const struct vm_string *string, const char *name);
static int svg_number(const char **cursor, float *value);
static void svg_skip(const char **cursor);
static float svg_length(const char *text, int *valid);
static int svg_parse_color(const char *text, uint32_t *color, int *none);
static void svg_style_apply(struct svg_style *style, const char *name, const char *value);
static void svg_style_read(const struct dom_element *element, struct svg_style *style, int *hidden);
static int svg_matrix_parse(const char *text, struct svg_matrix *matrix);
static void svg_matrix_multiply(struct svg_matrix *result, const struct svg_matrix *left, const struct svg_matrix *right);
static int svg_walk(struct svg_canvas *canvas, const struct dom_node *node, const struct svg_matrix *parent, const struct svg_style *inherited, int depth);
static int svg_draw_element(struct svg_canvas *canvas, const struct dom_element *element, const struct svg_matrix *matrix, const struct svg_style *style);
static int svg_draw_use(struct svg_canvas *canvas, const struct dom_element *element, const struct svg_matrix *matrix, const struct svg_style *style, int depth);
static int svg_draw_node(struct svg_canvas *canvas, const struct dom_element *element, const struct svg_matrix *matrix, const struct svg_style *style, int depth);
static int svg_draw_clipped(struct svg_canvas *canvas, const struct dom_element *element, const struct svg_matrix *matrix, const struct svg_style *style, const struct dom_element *clip, int depth);
static const struct dom_element *svg_clip_path_of(const struct dom_element *element);
static int svg_shape_point(struct svg_shape *shape, const struct svg_matrix *matrix, float x, float y, int begin);
static int svg_shape_path(struct svg_shape *shape, const struct svg_matrix *matrix, const char *text);
static int svg_shape_arc(struct svg_shape *shape, const struct svg_matrix *matrix, float x0, float y0, float rx, float ry, float rotation, int large, int sweep, float x1, float y1);
static int svg_shape_ellipse(struct svg_shape *shape, const struct svg_matrix *matrix, float cx, float cy, float rx, float ry);
static int svg_shape_rect(struct svg_shape *shape, const struct svg_matrix *matrix, float x, float y, float width, float height, float rx, float ry);
static int svg_shape_points(struct svg_shape *shape, const struct svg_matrix *matrix, const char *text);
static int svg_fill(struct svg_canvas *canvas, const struct svg_shape *shape, uint32_t color, float alpha, enum svg_rule rule);
static int svg_stroke(struct svg_canvas *canvas, const struct svg_shape *shape, const struct svg_matrix *matrix, const struct svg_style *style, uint32_t color, float alpha);
static void svg_composite_row(struct svg_canvas *canvas, int row, uint32_t color, float alpha);
static struct dom_element *svg_find_root(struct dom_node *node, int depth);
static const struct dom_element *svg_find_id(const struct dom_node *node, const char *id, int depth);
static uint32_t svg_gradient_color(const struct dom_element *svg, const char *reference, uint32_t fallback, int *found);
static float svg_parse_attribute_number(const struct dom_element *element, const char *name, float fallback);
static float svg_percent_length(const char *text, const char *name, float fallback);

/*
 * Draws an <svg> element into a new bitmap at its width and height (its
 * viewBox's size when it has none).
 *
 * Returns 0, or an errno value (EINVAL for an svg with no area).
 */
int
page_svg_render(
	const struct dom_element *svg,
	struct img_bitmap *bitmap)
{
	struct wb_buffer buffer;
	struct svg_canvas canvas;
	struct svg_matrix matrix;
	struct svg_style style;
	const char *text;
	float box[4];
	float width;
	float height;
	float view_width;
	float view_height;
	float scale;
	float scale_x;
	float scale_y;
	float shift_x;
	float shift_y;
	float shrink;
	int have_view;
	int no_size;
	int has_width;
	int has_height;
	int natural_set;
	int natural_width;
	int natural_height;
	int valid;
	int pixel_width;
	int pixel_height;
	int count;
	int row;
	int column;
	int error;
	size_t index;

	/* The viewBox, when it has four numbers and a size. */
	have_view = 0;
	view_width = 0.0f;
	view_height = 0.0f;
	box[0] = 0.0f;
	box[1] = 0.0f;
	box[2] = 0.0f;
	box[3] = 0.0f;
	wb_buffer_init(&buffer);
	text = svg_attribute(svg, "viewBox", &buffer);
	if (text != NULL) {
		count = 0;
		while (count < 4 && svg_number(&text, &box[count]))
			count++;
		if (count == 4 && box[2] > 0.0f && box[3] > 0.0f) {
			have_view = 1;
			view_width = box[2];
			view_height = box[3];
		}
	}
	wb_buffer_release(&buffer);

	/* The size: the attributes' pixels, else the viewBox's, else the default object size. */
	width = 0.0f;
	height = 0.0f;
	has_width = 0;
	has_height = 0;
	wb_buffer_init(&buffer);
	text = svg_attribute(svg, "width", &buffer);
	if (text != NULL) {
		width = svg_length(text, &valid);
		has_width = valid && width >= 0.0f;
		if (!valid)
			width = 0.0f;
	}
	wb_buffer_release(&buffer);
	wb_buffer_init(&buffer);
	text = svg_attribute(svg, "height", &buffer);
	if (text != NULL) {
		height = svg_length(text, &valid);
		has_height = valid && height >= 0.0f;
		if (!valid)
			height = 0.0f;
	}
	wb_buffer_release(&buffer);
	no_size = width <= 0.0f && height <= 0.0f && !have_view;
	natural_set = 0;
	natural_width = 0;
	natural_height = 0;
	if (!have_view && (has_width || has_height)) {
		/* Without a viewBox the attributes give the natural size (zero included), the default size the missing one; a zero or missing side leaves no ratio. */
		natural_set = 1;
		natural_width = has_width ? (int)(width + 0.5f) : (int)SVG_DEFAULT_WIDTH;
		natural_height = has_height ? (int)(height + 0.5f) : (int)SVG_DEFAULT_HEIGHT;
		no_size = !has_width || !has_height || width <= 0.0f || height <= 0.0f;
	} else if (!have_view) {
		natural_set = 1;
		natural_width = (int)SVG_DEFAULT_WIDTH;
		natural_height = (int)SVG_DEFAULT_HEIGHT;
	}
	if (width <= 0.0f && height <= 0.0f) {
		if (have_view) {
			width = view_width;
			height = view_height;
		} else {
			width = SVG_DEFAULT_WIDTH;
			height = SVG_DEFAULT_HEIGHT;
		}
	} else if (width <= 0.0f) {
		width = have_view ? height * view_width / view_height : SVG_DEFAULT_WIDTH;
		no_size = !have_view;
	} else if (height <= 0.0f) {
		height = have_view ? width / (view_width / view_height) : SVG_DEFAULT_HEIGHT;
		no_size = !have_view;
	}

	/* A size larger than a bitmap's bound is scaled down, keeping the ratio. */
	shrink = 1.0f;
	if (width > (float)SVG_SIDE_MAX)
		shrink = (float)SVG_SIDE_MAX / width;
	if (height * shrink > (float)SVG_SIDE_MAX)
		shrink = (float)SVG_SIDE_MAX / height;
	width *= shrink;
	height *= shrink;
	pixel_width = (int)(width + 0.5f);
	pixel_height = (int)(height + 0.5f);
	if (pixel_width < 1 || pixel_height < 1)
		return EINVAL;

	/* The transform from the viewBox to the bitmap: preserveAspectRatio's default (meet, centered). */
	matrix.a = 1.0f;
	matrix.b = 0.0f;
	matrix.c = 0.0f;
	matrix.d = 1.0f;
	matrix.e = 0.0f;
	matrix.f = 0.0f;
	scale_x = shrink;
	scale_y = shrink;
	shift_x = 0.0f;
	shift_y = 0.0f;
	if (have_view) {
		scale_x = (float)pixel_width / view_width;
		scale_y = (float)pixel_height / view_height;
		scale = scale_x;
		if (scale_y < scale)
			scale = scale_y;
		scale_x = scale;
		scale_y = scale;
		shift_x = ((float)pixel_width - view_width * scale) / 2.0f - box[0] * scale;
		shift_y = ((float)pixel_height - view_height * scale) / 2.0f - box[1] * scale;
	}
	svg_view_width = have_view ? view_width : width;
	svg_view_height = have_view ? view_height : height;
	matrix.a = scale_x;
	matrix.d = scale_y;
	matrix.e = shift_x;
	matrix.f = shift_y;

	/* The canvas: transparent. */
	canvas.width = pixel_width;
	canvas.height = pixel_height;
	canvas.pixels = calloc((size_t)pixel_width * (size_t)pixel_height * 4U, sizeof(float));
	canvas.coverage = calloc((size_t)pixel_width + 2U, sizeof(float));
	if (canvas.pixels == NULL || canvas.coverage == NULL) {
		free(canvas.pixels);
		free(canvas.coverage);
		return ENOMEM;
	}

	/* The initial style: black fill, opaque. */
	style.fill = 0xff000000U;
	style.fill_none = 0;
	style.fill_opacity = 1.0f;
	style.opacity = 1.0f;
	style.rule = SVG_NONZERO;
	style.stroke = 0xff000000U;
	style.stroke_none = 1;
	style.stroke_width = 1.0f;
	style.stroke_opacity = 1.0f;
	style.stroke_cap = 0;

	/* The children, in document order. */
	error = svg_walk(&canvas, &svg->node, &matrix, &style, 0);
	if (error != 0) {
		free(canvas.pixels);
		free(canvas.coverage);
		return error;
	}

	/* The bitmap, straight alpha. */
	error = img_bitmap_create(bitmap, pixel_width, pixel_height);
	if (error != 0) {
		free(canvas.pixels);
		free(canvas.coverage);
		return error;
	}
	bitmap->no_ratio = no_size;
	bitmap->has_natural = natural_set;
	bitmap->natural_width = natural_width;
	bitmap->natural_height = natural_height;
	for (row = 0; row < pixel_height; row++) {
		for (column = 0; column < pixel_width; column++) {
			float *pixel;
			float alpha;
			uint32_t red;
			uint32_t green;
			uint32_t blue;
			uint32_t alpha_byte;

			/* Un-premultiplies one pixel. */
			index = ((size_t)row * (size_t)pixel_width + (size_t)column) * 4U;
			pixel = &canvas.pixels[index];
			alpha = pixel[3];
			if (alpha <= 0.0f) {
				bitmap->pixels[(size_t)row * (size_t)pixel_width + (size_t)column] = 0U;
				continue;
			}
			if (alpha > 1.0f)
				alpha = 1.0f;
			red = (uint32_t)(pixel[0] / alpha * 255.0f + 0.5f);
			green = (uint32_t)(pixel[1] / alpha * 255.0f + 0.5f);
			blue = (uint32_t)(pixel[2] / alpha * 255.0f + 0.5f);
			if (red > 255U)
				red = 255U;
			if (green > 255U)
				green = 255U;
			if (blue > 255U)
				blue = 255U;
			alpha_byte = (uint32_t)(alpha * 255.0f + 0.5f);
			bitmap->pixels[(size_t)row * (size_t)pixel_width + (size_t)column] = (alpha_byte << 24) | (red << 16) | (green << 8) | blue;
		}
	}

	/* Succeeded: the bitmap has the picture. */
	free(canvas.pixels);
	free(canvas.coverage);
	return 0;
}

/* Draws the children of a node, each with the transform and style it brings. */
static int
svg_walk(
	struct svg_canvas *canvas,
	const struct dom_node *node,
	const struct svg_matrix *parent,
	const struct svg_style *inherited,
	int depth)
{
	const struct dom_node *child;
	const struct dom_element *element;
	struct svg_matrix local;
	struct svg_matrix matrix;
	struct svg_style style;
	struct wb_buffer buffer;
	const struct dom_element *clip;
	const char *text;
	int hidden;
	int error;

	/* Nested too deep draws nothing more. */
	if (depth > SVG_DEPTH)
		return 0;

	for (child = node->first_child; child != NULL; child = child->next) {
		if (child->type != DOM_ELEMENT)
			continue;
		element = (const struct dom_element *)child;
		if (element->ns != DOM_NS_SVG)
			continue;

		/* What is not drawn: definitions and the elements that only describe. */
		if (svg_name_is(element, "defs") ||
		    svg_name_is(element, "clipPath") ||
		    svg_name_is(element, "mask") ||
		    svg_name_is(element, "linearGradient") ||
		    svg_name_is(element, "radialGradient") ||
		    svg_name_is(element, "filter") ||
		    svg_name_is(element, "style") ||
		    svg_name_is(element, "title") ||
		    svg_name_is(element, "desc") ||
		    svg_name_is(element, "foreignObject") ||
		    svg_name_is(element, "symbol") ||
		    svg_name_is(element, "pattern"))
			continue;

		/* Its transform, composed after the parent's. */
		matrix = *parent;
		wb_buffer_init(&buffer);
		text = svg_attribute(element, "transform", &buffer);
		if (text != NULL && svg_matrix_parse(text, &local)) {
			svg_matrix_multiply(&matrix, parent, &local);
		}
		wb_buffer_release(&buffer);

		/* Its style over the inherited. */
		style = *inherited;
		hidden = 0;
		svg_style_read(element, &style, &hidden);
		if (hidden)
			continue;

		/* A clip path (clip-path="url(#id)", as an attribute or in the style attribute) masks what the element draws. */
		clip = svg_clip_path_of(element);
		if (clip != NULL) {
			error = svg_draw_clipped(canvas, element, &matrix, &style, clip, depth);
		} else {
			error = svg_draw_node(canvas, element, &matrix, &style, depth);
		}
		if (error != 0)
			return error;
	}

	/* Succeeded: the children are drawn. */
	return 0;
}

/* Draws one shape element. */
static int
svg_draw_element(
	struct svg_canvas *canvas,
	const struct dom_element *element,
	const struct svg_matrix *matrix,
	const struct svg_style *style)
{
	struct svg_shape shape;
	struct wb_buffer buffer;
	const char *text;
	const struct dom_element *root;
	const struct dom_node *top;
	uint32_t color;
	float alpha;
	float x;
	float y;
	float width;
	float height;
	float rx;
	float ry;
	int found;
	int error;

	/* A shape with neither a fill nor a stroke draws nothing. */
	if (style->fill_none && style->stroke_none)
		return 0;
	memset(&shape, 0, sizeof(shape));
	error = 0;

	/* The outline by the kind of element. */
	if (svg_name_is(element, "path")) {
		wb_buffer_init(&buffer);
		text = svg_attribute(element, "d", &buffer);
		if (text != NULL)
			error = svg_shape_path(&shape, matrix, text);
		wb_buffer_release(&buffer);
	} else if (svg_name_is(element, "rect")) {
		x = svg_parse_attribute_number(element, "x", 0.0f);
		y = svg_parse_attribute_number(element, "y", 0.0f);
		width = svg_parse_attribute_number(element, "width", 0.0f);
		height = svg_parse_attribute_number(element, "height", 0.0f);
		rx = svg_parse_attribute_number(element, "rx", -1.0f);
		ry = svg_parse_attribute_number(element, "ry", -1.0f);
		if (rx < 0.0f)
			rx = ry;
		if (ry < 0.0f)
			ry = rx;
		if (rx < 0.0f)
			rx = 0.0f;
		if (ry < 0.0f)
			ry = 0.0f;
		if (width > 0.0f && height > 0.0f)
			error = svg_shape_rect(&shape, matrix, x, y, width, height, rx, ry);
	} else if (svg_name_is(element, "circle")) {
		x = svg_parse_attribute_number(element, "cx", 0.0f);
		y = svg_parse_attribute_number(element, "cy", 0.0f);
		rx = svg_parse_attribute_number(element, "r", 0.0f);
		if (rx > 0.0f)
			error = svg_shape_ellipse(&shape, matrix, x, y, rx, rx);
	} else if (svg_name_is(element, "ellipse")) {
		x = svg_parse_attribute_number(element, "cx", 0.0f);
		y = svg_parse_attribute_number(element, "cy", 0.0f);
		rx = svg_parse_attribute_number(element, "rx", 0.0f);
		ry = svg_parse_attribute_number(element, "ry", 0.0f);
		if (rx > 0.0f && ry > 0.0f)
			error = svg_shape_ellipse(&shape, matrix, x, y, rx, ry);
	} else if (svg_name_is(element, "polygon") || svg_name_is(element, "polyline")) {
		wb_buffer_init(&buffer);
		text = svg_attribute(element, "points", &buffer);
		if (text != NULL)
			error = svg_shape_points(&shape, matrix, text);
		wb_buffer_release(&buffer);
		if (error == 0 && shape.count > 0 && svg_name_is(element, "polygon"))
			shape.points[0].closed = 1;
	} else if (svg_name_is(element, "line")) {
		x = svg_parse_attribute_number(element, "x1", 0.0f);
		y = svg_parse_attribute_number(element, "y1", 0.0f);
		width = svg_parse_attribute_number(element, "x2", 0.0f);
		height = svg_parse_attribute_number(element, "y2", 0.0f);
		error = svg_shape_point(&shape, matrix, x, y, 1);
		if (error == 0)
			error = svg_shape_point(&shape, matrix, width, height, 0);
	}
	if (error == 0 && shape.count > 0 && (svg_name_is(element, "rect") || svg_name_is(element, "circle") || svg_name_is(element, "ellipse")))
		shape.points[0].closed = 1;
	if (error != 0) {
		free(shape.points);
		return error;
	}
	if (shape.count < 2U) {
		free(shape.points);
		return 0;
	}

	/* A gradient fill takes its first stop's color, looked up from the outermost svg. */
	color = style->fill;
	alpha = style->fill_opacity * style->opacity;
	wb_buffer_init(&buffer);
	text = svg_attribute(element, "fill", &buffer);
	if (text != NULL && strncmp(text, "url(", 4) == 0) {
		top = &element->node;
		while (top->parent != NULL && top->parent->type == DOM_ELEMENT)
			top = top->parent;
		root = (const struct dom_element *)top;
		color = svg_gradient_color(root, text, color, &found);
		if (!found)
			alpha = 0.0f;
	}
	wb_buffer_release(&buffer);

	/* Filled. */
	if (alpha > 0.0f && !style->fill_none && shape.count >= 3U)
		error = svg_fill(canvas, &shape, color, alpha, style->rule);

	/* Then stroked, over the fill. */
	if (error == 0 && !style->stroke_none && style->stroke_width > 0.0f) {
		color = style->stroke;
		alpha = style->stroke_opacity * style->opacity;
		wb_buffer_init(&buffer);
		text = svg_attribute(element, "stroke", &buffer);
		if (text != NULL && strncmp(text, "url(", 4) == 0) {
			top = &element->node;
			while (top->parent != NULL && top->parent->type == DOM_ELEMENT)
				top = top->parent;
			root = (const struct dom_element *)top;
			color = svg_gradient_color(root, text, color, &found);
			if (!found)
				alpha = 0.0f;
		}
		wb_buffer_release(&buffer);
		if (alpha > 0.0f)
			error = svg_stroke(canvas, &shape, matrix, style, color, alpha);
	}
	free(shape.points);
	return error;
}

/* Finds the first stop's color of the gradient a url(#id) names; found is 0 when none is. */
static uint32_t
svg_gradient_color(
	const struct dom_element *svg,
	const char *reference,
	uint32_t fallback,
	int *found)
{
	const struct dom_element *gradient;
	const struct dom_node *child;
	const struct dom_element *stop;
	struct wb_buffer buffer;
	char identifier[128];
	const char *text;
	size_t length;
	uint32_t color;
	int none;

	/* The id inside url(#...). */
	*found = 0;
	text = strchr(reference, '#');
	if (text == NULL)
		return fallback;
	text++;
	length = 0;
	while (text[length] != '\0' && text[length] != ')' && text[length] != '\'' && text[length] != '"' && length < sizeof(identifier) - 1U) {
		identifier[length] = text[length];
		length++;
	}
	identifier[length] = '\0';

	/* The gradient element, and its first stop. */
	gradient = svg_find_id(&svg->node, identifier, 0);
	if (gradient == NULL)
		return fallback;
	for (child = gradient->node.first_child; child != NULL; child = child->next) {
		if (child->type != DOM_ELEMENT)
			continue;
		stop = (const struct dom_element *)child;
		if (!svg_name_is(stop, "stop"))
			continue;

		/* Its stop-color, from the attribute or the style attribute. */
		wb_buffer_init(&buffer);
		text = svg_attribute(stop, "stop-color", &buffer);
		color = fallback;
		none = 0;
		if (text != NULL && svg_parse_color(text, &color, &none) && !none)
			*found = 1;
		wb_buffer_release(&buffer);
		wb_buffer_init(&buffer);
		text = svg_attribute(stop, "style", &buffer);
		if (text != NULL && strstr(text, "stop-color:") != NULL) {
			const char *value;

			value = strstr(text, "stop-color:") + 11;
			while (*value == ' ')
				value++;
			if (svg_parse_color(value, &color, &none) && !none)
				*found = 1;
		}
		wb_buffer_release(&buffer);
		return color;
	}

	/* A gradient without a stop. */
	return fallback;
}

/* Finds the element with an id under a node. */
static const struct dom_element *
svg_find_id(
	const struct dom_node *node,
	const char *identifier,
	int depth)
{
	const struct dom_node *child;
	const struct dom_element *element;
	const struct dom_element *found;
	struct wb_buffer buffer;
	const char *text;
	int same;

	/* Not too deep. */
	if (depth > SVG_DEPTH * 4)
		return NULL;

	for (child = node->first_child; child != NULL; child = child->next) {
		if (child->type != DOM_ELEMENT)
			continue;
		element = (const struct dom_element *)child;

		/* This element's id. */
		wb_buffer_init(&buffer);
		text = svg_attribute(element, "id", &buffer);
		same = text != NULL && strcmp(text, identifier) == 0;
		wb_buffer_release(&buffer);
		if (same)
			return element;

		/* Its descendants'. */
		found = svg_find_id(child, identifier, depth + 1);
		if (found != NULL)
			return found;
	}
	return NULL;
}

/* Reads a presentation attribute as a number (its unit ignored), or the fallback. */
static float
svg_parse_attribute_number(
	const struct dom_element *element,
	const char *name,
	float fallback)
{
	struct wb_buffer buffer;
	const char *text;
	float value;
	int valid;

	wb_buffer_init(&buffer);
	text = svg_attribute(element, name, &buffer);
	value = fallback;
	if (text != NULL) {
		value = svg_length(text, &valid);
		if (!valid)
			value = svg_percent_length(text, name, fallback);
	}
	wb_buffer_release(&buffer);
	return value;
}

/* Reads a percentage as the share of the viewport's width, height or (for a radius) normalized diagonal; else the fallback. */
static float
svg_percent_length(
	const char *text,
	const char *name,
	float fallback)
{
	const char *cursor;
	float percent;
	float basis;

	cursor = text;
	if (!svg_number(&cursor, &percent) || *cursor != '%')
		return fallback;
	basis = svg_view_width;
	if (name[0] == 'y' || name[0] == 'h' || strcmp(name, "cy") == 0 || strcmp(name, "ry") == 0)
		basis = svg_view_height;
	else if (strcmp(name, "r") == 0)
		basis = sqrtf((svg_view_width * svg_view_width + svg_view_height * svg_view_height) / 2.0f);
	return percent * basis / 100.0f;
}

/*
 * Reads an element's attribute as UTF-8 text in the buffer (the name is matched
 * ignoring case); NULL when it has none.
 */
static const char *
svg_attribute(
	const struct dom_element *element,
	const char *name,
	struct wb_buffer *buffer)
{
	size_t index;
	int error;

	for (index = 0; index < element->attribute_count; index++) {
		if (element->attributes[index].ns != DOM_NS_NONE)
			continue;
		if (!svg_same_name(element->attributes[index].name, name))
			continue;
		error = vm_string_to_utf8(element->attributes[index].value, buffer);
		if (error != 0)
			return NULL;
		return wb_buffer_string(buffer);
	}
	return NULL;
}

/* Compares a string with an ASCII name ignoring case. */
static int
svg_same_name(
	const struct vm_string *string,
	const char *name)
{
	size_t length;
	size_t index;
	uint16_t unit;
	char wanted;

	length = strlen(name);
	if (string->length != length)
		return 0;
	for (index = 0; index < length; index++) {
		unit = vm_string_at(string, index);
		wanted = name[index];
		if (unit >= 'A' && unit <= 'Z')
			unit = (uint16_t)(unit - 'A' + 'a');
		if (wanted >= 'A' && wanted <= 'Z')
			wanted = (char)(wanted - 'A' + 'a');
		if (unit != (uint16_t)wanted)
			return 0;
	}
	return 1;
}

/* Tells whether an element's local name is the name (ignoring case). */
static int
svg_name_is(
	const struct dom_element *element,
	const char *name)
{
	return svg_same_name(element->local_name, name);
}

/* Skips white space and commas. */
static void
svg_skip(
	const char **cursor)
{
	while (**cursor == ' ' || **cursor == '\t' || **cursor == '\n' || **cursor == '\r' || **cursor == ',')
		(*cursor)++;
}

/* Reads a number (a sign, digits, a fraction and an exponent) after white space; 0 when there is none. */
static int
svg_number(
	const char **cursor,
	float *value)
{
	const char *start;
	const char *scan;
	char *end;
	double parsed;

	svg_skip(cursor);
	start = *cursor;
	scan = start;
	if (*scan == '+' || *scan == '-')
		scan++;
	if (!((*scan >= '0' && *scan <= '9') || (*scan == '.' && scan[1] >= '0' && scan[1] <= '9')))
		return 0;

	/* The longest number: digits, one point, digits, an exponent. */
	while (*scan >= '0' && *scan <= '9')
		scan++;
	if (*scan == '.') {
		scan++;
		while (*scan >= '0' && *scan <= '9')
			scan++;
	}
	if ((*scan == 'e' || *scan == 'E') &&
	    ((scan[1] >= '0' && scan[1] <= '9') || ((scan[1] == '-' || scan[1] == '+') && scan[2] >= '0' && scan[2] <= '9'))) {
		scan += 2;
		while (*scan >= '0' && *scan <= '9')
			scan++;
	}
	parsed = strtod(start, &end);
	if (end == start)
		return 0;
	*value = (float)parsed;
	*cursor = scan;
	return 1;
}

/* Reads a length: a number with an optional unit (px, pt, em...), a percentage being invalid. */
static float
svg_length(
	const char *text,
	int *valid)
{
	const char *cursor;
	float value;

	cursor = text;
	*valid = 0;
	if (!svg_number(&cursor, &value))
		return 0.0f;
	if (*cursor == '%')
		return 0.0f;
	if (cursor[0] == 'p' && cursor[1] == 't')
		value *= 4.0f / 3.0f;
	else if (cursor[0] == 'e' && cursor[1] == 'm')
		value *= 16.0f;
	*valid = 1;
	return value;
}

/* Parses a color: #rgb, #rrggbb, rgb(), rgba(), a name, none or currentColor. */
static int
svg_parse_color(
	const char *text,
	uint32_t *color,
	int *none)
{
	const char *cursor;
	float parts[4];
	unsigned long digits;
	size_t length;
	size_t index;
	int count;
	char *end;
	char buffer[16];

	*none = 0;
	cursor = text;
	while (*cursor == ' ')
		cursor++;

	/* None. */
	if (strncmp(cursor, "none", 4) == 0) {
		*none = 1;
		return 1;
	}

	/* currentColor is black here (the CSS color is not inherited into the picture). */
	if (strncmp(cursor, "currentColor", 12) == 0 || strncmp(cursor, "currentcolor", 12) == 0) {
		*color = 0xff000000U;
		return 1;
	}

	/* Hexadecimal. */
	if (*cursor == '#') {
		cursor++;
		length = 0;
		while ((cursor[length] >= '0' && cursor[length] <= '9') ||
		       (cursor[length] >= 'a' && cursor[length] <= 'f') ||
		       (cursor[length] >= 'A' && cursor[length] <= 'F'))
			length++;
		if (length == 3U || length == 4U) {
			digits = 0;
			for (index = 0; index < length; index++) {
				buffer[0] = cursor[index];
				buffer[1] = cursor[index];
				buffer[2] = '\0';
				digits = (digits << 8) | strtoul(buffer, &end, 16);
			}
			if (length == 3U)
				*color = 0xff000000U | (uint32_t)digits;
			else
				*color = (((uint32_t)digits & 0xffU) << 24) | (((uint32_t)digits >> 8) & 0xffffffU);
			return 1;
		}
		if (length == 6U || length == 8U) {
			memcpy(buffer, cursor, length);
			buffer[length] = '\0';
			digits = strtoul(buffer, &end, 16);
			if (length == 6U)
				*color = 0xff000000U | (uint32_t)digits;
			else
				*color = (((uint32_t)digits & 0xffU) << 24) | (((uint32_t)digits >> 8) & 0xffffffU);
			return 1;
		}
		return 0;
	}

	/* rgb() and rgba(). */
	if (strncmp(cursor, "rgb", 3) == 0) {
		cursor = strchr(cursor, '(');
		if (cursor == NULL)
			return 0;
		cursor++;
		parts[3] = 1.0f;
		for (count = 0; count < 4; count++) {
			if (!svg_number(&cursor, &parts[count]))
				break;
			if (*cursor == '%') {
				parts[count] = parts[count] * (count < 3 ? 2.55f : 0.01f);
				cursor++;
			}
			if (*cursor == '/')
				cursor++;
		}
		if (count < 3)
			return 0;
		for (index = 0; index < 3U; index++) {
			if (parts[index] < 0.0f)
				parts[index] = 0.0f;
			if (parts[index] > 255.0f)
				parts[index] = 255.0f;
		}
		if (parts[3] < 0.0f)
			parts[3] = 0.0f;
		if (parts[3] > 1.0f)
			parts[3] = 1.0f;
		*color = ((uint32_t)(parts[3] * 255.0f + 0.5f) << 24) | ((uint32_t)(parts[0] + 0.5f) << 16) |
			 ((uint32_t)(parts[1] + 0.5f) << 8) | (uint32_t)(parts[2] + 0.5f);
		return 1;
	}

	/* A name. */
	for (index = 0; index < sizeof(svg_colors) / sizeof(svg_colors[0]); index++) {
		length = strlen(svg_colors[index].name);
		if (strncmp(cursor, svg_colors[index].name, length) == 0 &&
		    (cursor[length] == '\0' || cursor[length] == ';' || cursor[length] == ' ')) {
			*color = svg_colors[index].value;
			return 1;
		}
	}
	return 0;
}

/* Applies one presentation property to a style. */
static void
svg_style_apply(
	struct svg_style *style,
	const char *name,
	const char *value)
{
	uint32_t color;
	float number;
	const char *cursor;
	int none;

	if (strcmp(name, "fill") == 0) {
		if (strncmp(value, "url(", 4) == 0) {
			style->fill_none = 0;
		} else if (svg_parse_color(value, &color, &none)) {
			style->fill_none = none;
			if (!none)
				style->fill = color;
		}
	} else if (strcmp(name, "fill-opacity") == 0 || strcmp(name, "opacity") == 0) {
		cursor = value;
		if (svg_number(&cursor, &number)) {
			if (*cursor == '%')
				number /= 100.0f;
			if (number < 0.0f)
				number = 0.0f;
			if (number > 1.0f)
				number = 1.0f;
			if (name[0] == 'o')
				style->opacity = number;
			else
				style->fill_opacity = number;
		}
	} else if (strcmp(name, "stroke") == 0) {
		if (strncmp(value, "url(", 4) == 0) {
			style->stroke_none = 0;
		} else if (svg_parse_color(value, &color, &none)) {
			style->stroke_none = none;
			if (!none)
				style->stroke = color;
		}
	} else if (strcmp(name, "stroke-width") == 0) {
		cursor = value;
		if (svg_number(&cursor, &number) && number >= 0.0f)
			style->stroke_width = number;
	} else if (strcmp(name, "stroke-opacity") == 0) {
		cursor = value;
		if (svg_number(&cursor, &number)) {
			if (*cursor == '%')
				number /= 100.0f;
			if (number < 0.0f)
				number = 0.0f;
			if (number > 1.0f)
				number = 1.0f;
			style->stroke_opacity = number;
		}
	} else if (strcmp(name, "stroke-linecap") == 0) {
		if (strncmp(value, "round", 5) == 0)
			style->stroke_cap = 1;
		else if (strncmp(value, "square", 6) == 0)
			style->stroke_cap = 2;
		else if (strncmp(value, "butt", 4) == 0)
			style->stroke_cap = 0;
	} else if (strcmp(name, "fill-rule") == 0) {
		if (strncmp(value, "evenodd", 7) == 0)
			style->rule = SVG_EVENODD;
		else if (strncmp(value, "nonzero", 7) == 0)
			style->rule = SVG_NONZERO;
	}
}

/* Reads an element's presentation attributes and its style attribute into a style; hidden when it is not drawn. */
static void
svg_style_read(
	const struct dom_element *element,
	struct svg_style *style,
	int *hidden)
{
	static const char *names[] = { "fill", "fill-opacity", "opacity", "fill-rule", "stroke", "stroke-width", "stroke-opacity", "stroke-linecap" };
	struct wb_buffer buffer;
	char declarations[1024];
	char name[32];
	char value[128];
	const char *text;
	const char *cursor;
	size_t index;
	size_t length;

	/* The attributes. */
	for (index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
		wb_buffer_init(&buffer);
		text = svg_attribute(element, names[index], &buffer);
		if (text != NULL)
			svg_style_apply(style, names[index], text);
		wb_buffer_release(&buffer);
	}

	/* display and visibility. */
	wb_buffer_init(&buffer);
	text = svg_attribute(element, "display", &buffer);
	if (text != NULL && strncmp(text, "none", 4) == 0)
		*hidden = 1;
	wb_buffer_release(&buffer);

	/* The style attribute's declarations override. */
	wb_buffer_init(&buffer);
	text = svg_attribute(element, "style", &buffer);
	if (text == NULL) {
		wb_buffer_release(&buffer);
		return;
	}
	length = strlen(text);
	if (length >= sizeof(declarations))
		length = sizeof(declarations) - 1U;
	memcpy(declarations, text, length);
	declarations[length] = '\0';
	wb_buffer_release(&buffer);
	cursor = declarations;
	while (*cursor != '\0') {
		size_t count;

		/* name: value; */
		while (*cursor == ' ' || *cursor == ';')
			cursor++;
		count = 0;
		while (*cursor != '\0' && *cursor != ':' && *cursor != ';') {
			if (count < sizeof(name) - 1U) {
				name[count] = *cursor;
				count++;
			}
			cursor++;
		}
		name[count] = '\0';
		while (count > 0U && name[count - 1U] == ' ') {
			count--;
			name[count] = '\0';
		}
		if (*cursor != ':')
			break;
		cursor++;
		while (*cursor == ' ')
			cursor++;
		count = 0;
		while (*cursor != '\0' && *cursor != ';') {
			if (count < sizeof(value) - 1U) {
				value[count] = *cursor;
				count++;
			}
			cursor++;
		}
		value[count] = '\0';
		if (strcmp(name, "display") == 0 && strncmp(value, "none", 4) == 0)
			*hidden = 1;
		svg_style_apply(style, name, value);
	}
}

/* result = left * right (the right is applied first). */
static void
svg_matrix_multiply(
	struct svg_matrix *result,
	const struct svg_matrix *left,
	const struct svg_matrix *right)
{
	struct svg_matrix product;

	product.a = left->a * right->a + left->c * right->b;
	product.b = left->b * right->a + left->d * right->b;
	product.c = left->a * right->c + left->c * right->d;
	product.d = left->b * right->c + left->d * right->d;
	product.e = left->a * right->e + left->c * right->f + left->e;
	product.f = left->b * right->e + left->d * right->f + left->f;
	*result = product;
}

/* Parses a transform list (matrix, translate, scale, rotate, skewX, skewY); 0 when it has none. */
static int
svg_matrix_parse(
	const char *text,
	struct svg_matrix *matrix)
{
	struct svg_matrix part;
	struct svg_matrix next;
	float values[6];
	float angle;
	const char *cursor;
	char name[16];
	size_t length;
	int count;
	int found;

	matrix->a = 1.0f;
	matrix->b = 0.0f;
	matrix->c = 0.0f;
	matrix->d = 1.0f;
	matrix->e = 0.0f;
	matrix->f = 0.0f;
	found = 0;
	cursor = text;
	for (;;) {
		svg_skip(&cursor);
		length = 0;
		while ((*cursor >= 'a' && *cursor <= 'z') || (*cursor >= 'A' && *cursor <= 'Z')) {
			if (length < sizeof(name) - 1U) {
				name[length] = *cursor;
				length++;
			}
			cursor++;
		}
		name[length] = '\0';
		if (length == 0U || *cursor != '(')
			break;
		cursor++;
		count = 0;
		while (count < 6 && svg_number(&cursor, &values[count]))
			count++;
		svg_skip(&cursor);
		if (*cursor != ')')
			break;
		cursor++;

		/* This function's matrix. */
		part.a = 1.0f;
		part.b = 0.0f;
		part.c = 0.0f;
		part.d = 1.0f;
		part.e = 0.0f;
		part.f = 0.0f;
		if (strcmp(name, "matrix") == 0 && count == 6) {
			part.a = values[0];
			part.b = values[1];
			part.c = values[2];
			part.d = values[3];
			part.e = values[4];
			part.f = values[5];
		} else if (strcmp(name, "translate") == 0 && count >= 1) {
			part.e = values[0];
			part.f = count >= 2 ? values[1] : 0.0f;
		} else if (strcmp(name, "scale") == 0 && count >= 1) {
			part.a = values[0];
			part.d = count >= 2 ? values[1] : values[0];
		} else if (strcmp(name, "rotate") == 0 && count >= 1) {
			angle = values[0] * 3.14159265f / 180.0f;
			part.a = cosf(angle);
			part.b = sinf(angle);
			part.c = -sinf(angle);
			part.d = cosf(angle);
			if (count == 3) {
				part.e = values[1] - part.a * values[1] - part.c * values[2];
				part.f = values[2] - part.b * values[1] - part.d * values[2];
			}
		} else if (strcmp(name, "skewX") == 0 && count >= 1) {
			part.c = tanf(values[0] * 3.14159265f / 180.0f);
		} else if (strcmp(name, "skewY") == 0 && count >= 1) {
			part.b = tanf(values[0] * 3.14159265f / 180.0f);
		} else {
			break;
		}
		svg_matrix_multiply(&next, matrix, &part);
		*matrix = next;
		found = 1;
	}
	return found;
}

/* Adds a point to a shape, transformed into device space. */
static int
svg_shape_point(
	struct svg_shape *shape,
	const struct svg_matrix *matrix,
	float x,
	float y,
	int begin)
{
	struct svg_point *grown;
	size_t capacity;

	if (shape->count >= SVG_POINTS_MAX)
		return 0;
	if (shape->count == shape->capacity) {
		capacity = shape->capacity == 0U ? 64U : shape->capacity * 2U;
		grown = realloc(shape->points, capacity * sizeof(*grown));
		if (grown == NULL)
			return ENOMEM;
		shape->points = grown;
		shape->capacity = capacity;
	}
	shape->points[shape->count].x = matrix->a * x + matrix->c * y + matrix->e;
	shape->points[shape->count].y = matrix->b * x + matrix->d * y + matrix->f;
	shape->points[shape->count].begin = begin;
	shape->points[shape->count].closed = 0;
	shape->count++;
	return 0;
}

/* Adds an elliptical arc, as the SVG implementation notes' endpoint-to-center conversion gives it. */
static int
svg_shape_arc(
	struct svg_shape *shape,
	const struct svg_matrix *matrix,
	float x0,
	float y0,
	float rx,
	float ry,
	float rotation,
	int large,
	int sweep,
	float x1,
	float y1)
{
	double cos_phi;
	double sin_phi;
	double dx;
	double dy;
	double x1p;
	double y1p;
	double lambda;
	double numerator;
	double denominator;
	double factor;
	double cxp;
	double cyp;
	double cx;
	double cy;
	double theta;
	double delta;
	double ux;
	double uy;
	double vx;
	double vy;
	double angle;
	int steps;
	int index;
	int error;

	/* A zero radius or no movement is a line. */
	rx = fabsf(rx);
	ry = fabsf(ry);
	if ((x0 == x1 && y0 == y1))
		return 0;
	if (rx == 0.0f || ry == 0.0f)
		return svg_shape_point(shape, matrix, x1, y1, 0);

	cos_phi = cos((double)rotation * 3.14159265358979 / 180.0);
	sin_phi = sin((double)rotation * 3.14159265358979 / 180.0);
	dx = ((double)x0 - (double)x1) / 2.0;
	dy = ((double)y0 - (double)y1) / 2.0;
	x1p = cos_phi * dx + sin_phi * dy;
	y1p = -sin_phi * dx + cos_phi * dy;

	/* Radii too small to reach are scaled up. */
	lambda = (x1p * x1p) / ((double)rx * rx) + (y1p * y1p) / ((double)ry * ry);
	if (lambda > 1.0) {
		rx = (float)((double)rx * sqrt(lambda));
		ry = (float)((double)ry * sqrt(lambda));
	}

	/* The center. */
	numerator = (double)rx * rx * ry * ry - (double)rx * rx * y1p * y1p - (double)ry * ry * x1p * x1p;
	denominator = (double)rx * rx * y1p * y1p + (double)ry * ry * x1p * x1p;
	factor = 0.0;
	if (denominator != 0.0 && numerator > 0.0)
		factor = sqrt(numerator / denominator);
	if (large == sweep)
		factor = -factor;
	cxp = factor * (double)rx * y1p / (double)ry;
	cyp = -factor * (double)ry * x1p / (double)rx;
	cx = cos_phi * cxp - sin_phi * cyp + ((double)x0 + (double)x1) / 2.0;
	cy = sin_phi * cxp + cos_phi * cyp + ((double)y0 + (double)y1) / 2.0;

	/* The start angle and the sweep. */
	ux = (x1p - cxp) / (double)rx;
	uy = (y1p - cyp) / (double)ry;
	vx = (-x1p - cxp) / (double)rx;
	vy = (-y1p - cyp) / (double)ry;
	theta = atan2(uy, ux);
	delta = atan2(vy, vx) - theta;
	if (!sweep && delta > 0.0)
		delta -= 2.0 * 3.14159265358979;
	else if (sweep && delta < 0.0)
		delta += 2.0 * 3.14159265358979;

	/* Points along it. */
	steps = (int)(fabs(delta) / (3.14159265358979 / 2.0) * (double)SVG_CURVE_STEPS) + 1;
	for (index = 1; index <= steps; index++) {
		angle = theta + delta * (double)index / (double)steps;
		error = svg_shape_point(shape, matrix,
					(float)(cx + (double)rx * cos(angle) * cos_phi - (double)ry * sin(angle) * sin_phi),
					(float)(cy + (double)rx * cos(angle) * sin_phi + (double)ry * sin(angle) * cos_phi),
					0);
		if (error != 0)
			return error;
	}
	return svg_shape_point(shape, matrix, x1, y1, 0);
}

/* Adds a closed ellipse. */
static int
svg_shape_ellipse(
	struct svg_shape *shape,
	const struct svg_matrix *matrix,
	float cx,
	float cy,
	float rx,
	float ry)
{
	int steps;
	int index;
	int error;
	double angle;

	steps = SVG_CURVE_STEPS * 4;
	for (index = 0; index < steps; index++) {
		angle = 2.0 * 3.14159265358979 * (double)index / (double)steps;
		error = svg_shape_point(shape, matrix, cx + rx * (float)cos(angle), cy + ry * (float)sin(angle), index == 0);
		if (error != 0)
			return error;
	}
	return 0;
}

/* Adds a rectangle, its corners rounded by rx and ry. */
static int
svg_shape_rect(
	struct svg_shape *shape,
	const struct svg_matrix *matrix,
	float x,
	float y,
	float width,
	float height,
	float rx,
	float ry)
{
	double angle;
	float corner_x;
	float corner_y;
	int corner;
	int step;
	int error;

	/* The radii are at most half of the sides. */
	if (rx > width / 2.0f)
		rx = width / 2.0f;
	if (ry > height / 2.0f)
		ry = height / 2.0f;

	/* Square corners. */
	if (rx <= 0.0f || ry <= 0.0f) {
		error = svg_shape_point(shape, matrix, x, y, 1);
		if (error == 0)
			error = svg_shape_point(shape, matrix, x + width, y, 0);
		if (error == 0)
			error = svg_shape_point(shape, matrix, x + width, y + height, 0);
		if (error == 0)
			error = svg_shape_point(shape, matrix, x, y + height, 0);
		return error;
	}

	/* Four quarter ellipses, from the top right clockwise. */
	for (corner = 0; corner < 4; corner++) {
		corner_x = (corner == 0 || corner == 1) ? x + width - rx : x + rx;
		corner_y = (corner == 0 || corner == 3) ? y + ry : y + height - ry;
		for (step = 0; step <= SVG_CURVE_STEPS; step++) {
			angle = ((double)corner * 90.0 - 90.0 + 90.0 * (double)step / (double)SVG_CURVE_STEPS) * 3.14159265358979 / 180.0;
			error = svg_shape_point(shape, matrix, corner_x + rx * (float)cos(angle), corner_y + ry * (float)sin(angle), corner == 0 && step == 0);
			if (error != 0)
				return error;
		}
	}
	return 0;
}

/* Adds the points of a polygon's points attribute. */
static int
svg_shape_points(
	struct svg_shape *shape,
	const struct svg_matrix *matrix,
	const char *text)
{
	const char *cursor;
	float x;
	float y;
	int first;
	int error;

	cursor = text;
	first = 1;
	while (svg_number(&cursor, &x) && svg_number(&cursor, &y)) {
		error = svg_shape_point(shape, matrix, x, y, first);
		if (error != 0)
			return error;
		first = 0;
	}
	return 0;
}

/* Reads one arc flag (a single 0 or 1, which may touch the next number). */
static int
svg_flag(
	const char **cursor,
	int *flag)
{
	svg_skip(cursor);
	if (**cursor != '0' && **cursor != '1')
		return 0;
	*flag = **cursor == '1';
	(*cursor)++;
	return 1;
}

/* Adds the outline of a path's d attribute. */
static int
svg_shape_path(
	struct svg_shape *shape,
	const struct svg_matrix *matrix,
	const char *text)
{
	const char *cursor;
	char command;
	char previous;
	float x;
	float y;
	float start_x;
	float start_y;
	float control_x;
	float control_y;
	float values[7];
	float x1;
	float y1;
	float x2;
	float y2;
	float t;
	float u;
	int large;
	int sweep;
	int step;
	int relative;
	int count;
	int error;
	int need_begin;
	size_t begin_index;

	x = 0.0f;
	y = 0.0f;
	start_x = 0.0f;
	start_y = 0.0f;
	control_x = 0.0f;
	control_y = 0.0f;
	previous = 0;
	command = 0;
	need_begin = 1;
	cursor = text;
	for (;;) {
		svg_skip(&cursor);
		if (*cursor == '\0')
			break;

		/* A command letter, or the previous command repeated by more numbers. */
		if ((*cursor >= 'a' && *cursor <= 'z') || (*cursor >= 'A' && *cursor <= 'Z')) {
			command = *cursor;
			cursor++;
			if (command == 'z' || command == 'Z') {
				for (begin_index = shape->count; begin_index > 0; begin_index--) {
					if (shape->points[begin_index - 1U].begin) {
						shape->points[begin_index - 1U].closed = 1;
						break;
					}
				}
				x = start_x;
				y = start_y;
				need_begin = 1;
				previous = 'z';
				continue;
			}
		} else if (command == 0 || command == 'z' || command == 'Z') {
			break;
		} else if (command == 'M') {
			command = 'L';
		} else if (command == 'm') {
			command = 'l';
		}
		relative = command >= 'a';
		count = 0;
		switch (command | 0x20) {
		case 'm':
		case 'l':
		case 't':
			count = 2;
			break;
		case 'h':
		case 'v':
			count = 1;
			break;
		case 'c':
			count = 6;
			break;
		case 's':
		case 'q':
			count = 4;
			break;
		case 'a':
			count = 7;
			break;
		default:
			return 0;
		}

		/* The command's numbers (an arc's flags are single digits). */
		for (step = 0; step < count; step++) {
			if ((command | 0x20) == 'a' && (step == 3 || step == 4)) {
				int flag;

				flag = 0;
				if (!svg_flag(&cursor, &flag))
					return 0;
				values[step] = (float)flag;
			} else if (!svg_number(&cursor, &values[step])) {
				return 0;
			}
		}

		/* The command. */
		switch (command | 0x20) {
		case 'm':
			if (relative) {
				values[0] += x;
				values[1] += y;
			}
			x = values[0];
			y = values[1];
			start_x = x;
			start_y = y;
			error = svg_shape_point(shape, matrix, x, y, 1);
			need_begin = 0;
			break;
		case 'l':
		case 'h':
		case 'v':
			if ((command | 0x20) == 'h') {
				values[0] = relative ? x + values[0] : values[0];
				values[1] = y;
			} else if ((command | 0x20) == 'v') {
				values[1] = relative ? y + values[0] : values[0];
				values[0] = x;
			} else if (relative) {
				values[0] += x;
				values[1] += y;
			}
			error = 0;
			if (need_begin)
				error = svg_shape_point(shape, matrix, x, y, 1);
			need_begin = 0;
			x = values[0];
			y = values[1];
			if (error == 0)
				error = svg_shape_point(shape, matrix, x, y, 0);
			break;
		case 'c':
		case 's':
			if ((command | 0x20) == 'c') {
				x1 = relative ? x + values[0] : values[0];
				y1 = relative ? y + values[1] : values[1];
				x2 = relative ? x + values[2] : values[2];
				y2 = relative ? y + values[3] : values[3];
				values[4] = relative ? x + values[4] : values[4];
				values[5] = relative ? y + values[5] : values[5];
			} else {
				x1 = x;
				y1 = y;
				if (previous == 'c' || previous == 's') {
					x1 = 2.0f * x - control_x;
					y1 = 2.0f * y - control_y;
				}
				x2 = relative ? x + values[0] : values[0];
				y2 = relative ? y + values[1] : values[1];
				values[4] = relative ? x + values[2] : values[2];
				values[5] = relative ? y + values[3] : values[3];
			}
			error = 0;
			if (need_begin)
				error = svg_shape_point(shape, matrix, x, y, 1);
			need_begin = 0;
			for (step = 1; step <= SVG_CURVE_STEPS && error == 0; step++) {
				t = (float)step / (float)SVG_CURVE_STEPS;
				u = 1.0f - t;
				error = svg_shape_point(shape, matrix,
							u * u * u * x + 3.0f * u * u * t * x1 + 3.0f * u * t * t * x2 + t * t * t * values[4],
							u * u * u * y + 3.0f * u * u * t * y1 + 3.0f * u * t * t * y2 + t * t * t * values[5],
							0);
			}
			control_x = x2;
			control_y = y2;
			x = values[4];
			y = values[5];
			break;
		case 'q':
		case 't':
			if ((command | 0x20) == 'q') {
				x1 = relative ? x + values[0] : values[0];
				y1 = relative ? y + values[1] : values[1];
				values[2] = relative ? x + values[2] : values[2];
				values[3] = relative ? y + values[3] : values[3];
			} else {
				x1 = x;
				y1 = y;
				if (previous == 'q' || previous == 't') {
					x1 = 2.0f * x - control_x;
					y1 = 2.0f * y - control_y;
				}
				values[2] = relative ? x + values[0] : values[0];
				values[3] = relative ? y + values[1] : values[1];
			}
			error = 0;
			if (need_begin)
				error = svg_shape_point(shape, matrix, x, y, 1);
			need_begin = 0;
			for (step = 1; step <= SVG_CURVE_STEPS && error == 0; step++) {
				t = (float)step / (float)SVG_CURVE_STEPS;
				u = 1.0f - t;
				error = svg_shape_point(shape, matrix,
							u * u * x + 2.0f * u * t * x1 + t * t * values[2],
							u * u * y + 2.0f * u * t * y1 + t * t * values[3],
							0);
			}
			control_x = x1;
			control_y = y1;
			x = values[2];
			y = values[3];
			break;
		default:
			/* An arc. */
			large = values[3] != 0.0f;
			sweep = values[4] != 0.0f;
			if (relative) {
				values[5] += x;
				values[6] += y;
			}
			error = 0;
			if (need_begin)
				error = svg_shape_point(shape, matrix, x, y, 1);
			need_begin = 0;
			if (error == 0)
				error = svg_shape_arc(shape, matrix, x, y, values[0], values[1], values[2], large, sweep, values[5], values[6]);
			x = values[5];
			y = values[6];
			break;
		}
		if (error != 0)
			return error;
		previous = (char)(command | 0x20);
	}
	return 0;
}

/* Fills a shape (all subpaths together) with a color by scanlines, compositing into the canvas. */
static int
svg_fill(
	struct svg_canvas *canvas,
	const struct svg_shape *shape,
	uint32_t color,
	float alpha,
	enum svg_rule rule)
{
	struct svg_edge *edges;
	float *crossings_x;
	int *crossings_direction;
	size_t edge_count;
	size_t cross_count;
	size_t index;
	size_t inner;
	size_t first;
	size_t last;
	size_t next;
	size_t point;
	float top;
	float bottom;
	float scan_y;
	float x;
	float start;
	float end;
	float swap_x;
	int swap_direction;
	int row;
	int row_first;
	int row_last;
	int sub;
	int winding;
	int inside;
	int pixel;
	int pixel_end;
	int touched;

	/* The edges: every subpath is closed, horizontal edges dropped, the upper end first. */
	edges = malloc(shape->count * sizeof(*edges));
	crossings_x = malloc(shape->count * sizeof(float));
	crossings_direction = malloc(shape->count * sizeof(int));
	if (edges == NULL || crossings_x == NULL || crossings_direction == NULL) {
		free(edges);
		free(crossings_x);
		free(crossings_direction);
		return ENOMEM;
	}
	edge_count = 0;
	top = 1.0e9f;
	bottom = -1.0e9f;
	first = 0;
	while (first < shape->count) {
		/* One subpath: from its first point up to the next subpath's. */
		last = first + 1U;
		while (last < shape->count && !shape->points[last].begin)
			last++;
		for (point = first; point < last; point++) {
			/* The edge to the next point, the last one closing back to the first. */
			next = point + 1U;
			if (next >= last)
				next = first;
			if (shape->points[point].y == shape->points[next].y)
				continue;
			if (shape->points[point].y < shape->points[next].y) {
				edges[edge_count].x0 = shape->points[point].x;
				edges[edge_count].y0 = shape->points[point].y;
				edges[edge_count].x1 = shape->points[next].x;
				edges[edge_count].y1 = shape->points[next].y;
				edges[edge_count].direction = 1;
			} else {
				edges[edge_count].x0 = shape->points[next].x;
				edges[edge_count].y0 = shape->points[next].y;
				edges[edge_count].x1 = shape->points[point].x;
				edges[edge_count].y1 = shape->points[point].y;
				edges[edge_count].direction = -1;
			}
			if (edges[edge_count].y0 < top)
				top = edges[edge_count].y0;
			if (edges[edge_count].y1 > bottom)
				bottom = edges[edge_count].y1;
			edge_count++;
		}
		first = last;
	}

	/* The rows the shape touches. */
	row_first = (int)floorf(top);
	row_last = (int)ceilf(bottom);
	if (row_first < 0)
		row_first = 0;
	if (row_last > canvas->height)
		row_last = canvas->height;
	for (row = row_first; row < row_last; row++) {
		touched = 0;
		memset(canvas->coverage, 0, ((size_t)canvas->width + 2U) * sizeof(float));
		for (sub = 0; sub < SVG_SUBLINES; sub++) {
			scan_y = (float)row + ((float)sub + 0.5f) / (float)SVG_SUBLINES;

			/* The edges the sub-scanline crosses, by x. */
			cross_count = 0;
			for (index = 0; index < edge_count; index++) {
				if (scan_y < edges[index].y0 || scan_y >= edges[index].y1)
					continue;
				x = edges[index].x0 + (scan_y - edges[index].y0) * (edges[index].x1 - edges[index].x0) / (edges[index].y1 - edges[index].y0);
				crossings_x[cross_count] = x;
				crossings_direction[cross_count] = edges[index].direction;
				cross_count++;
			}
			for (index = 1; index < cross_count; index++) {
				for (inner = index; inner > 0U && crossings_x[inner - 1U] > crossings_x[inner]; inner--) {
					swap_x = crossings_x[inner];
					crossings_x[inner] = crossings_x[inner - 1U];
					crossings_x[inner - 1U] = swap_x;
					swap_direction = crossings_direction[inner];
					crossings_direction[inner] = crossings_direction[inner - 1U];
					crossings_direction[inner - 1U] = swap_direction;
				}
			}

			/* The spans inside, by the rule. */
			winding = 0;
			for (index = 0; index + 1U < cross_count; index++) {
				winding += crossings_direction[index];
				inside = rule == SVG_EVENODD ? ((index & 1U) == 0U) : (winding != 0);
				if (!inside)
					continue;
				start = crossings_x[index];
				end = crossings_x[index + 1U];
				if (end <= 0.0f || start >= (float)canvas->width)
					continue;
				if (start < 0.0f)
					start = 0.0f;
				if (end > (float)canvas->width)
					end = (float)canvas->width;
				pixel = (int)start;
				pixel_end = (int)end;
				touched = 1;
				if (pixel == pixel_end) {
					canvas->coverage[pixel] += (end - start) / (float)SVG_SUBLINES;
				} else {
					canvas->coverage[pixel] += ((float)(pixel + 1) - start) / (float)SVG_SUBLINES;
					for (pixel++; pixel < pixel_end; pixel++)
						canvas->coverage[pixel] += 1.0f / (float)SVG_SUBLINES;
					canvas->coverage[pixel_end] += (end - (float)pixel_end) / (float)SVG_SUBLINES;
				}
			}
		}
		if (touched)
			svg_composite_row(canvas, row, color, alpha);
	}

	/* Succeeded: the shape is drawn. */
	free(edges);
	free(crossings_x);
	free(crossings_direction);
	return 0;
}

/* Composites a color over a row with the row's coverage (premultiplied "over"). */
static void
svg_composite_row(
	struct svg_canvas *canvas,
	int row,
	uint32_t color,
	float alpha)
{
	float *pixel;
	float source_alpha;
	float red;
	float green;
	float blue;
	float cover;
	int column;

	red = (float)((color >> 16) & 0xffU) / 255.0f;
	green = (float)((color >> 8) & 0xffU) / 255.0f;
	blue = (float)(color & 0xffU) / 255.0f;
	alpha *= (float)(color >> 24) / 255.0f;
	for (column = 0; column < canvas->width; column++) {
		cover = canvas->coverage[column];
		if (cover <= 0.0f)
			continue;
		if (cover > 1.0f)
			cover = 1.0f;
		source_alpha = cover * alpha;
		pixel = &canvas->pixels[((size_t)row * (size_t)canvas->width + (size_t)column) * 4U];
		pixel[0] = red * source_alpha + pixel[0] * (1.0f - source_alpha);
		pixel[1] = green * source_alpha + pixel[1] * (1.0f - source_alpha);
		pixel[2] = blue * source_alpha + pixel[2] * (1.0f - source_alpha);
		pixel[3] = source_alpha + pixel[3] * (1.0f - source_alpha);
	}
}

/*
 * Draws the bytes of an SVG image (an <img>, a CSS url() or a data: URL that
 * names an SVG document) into a bitmap: the text is parsed into a scratch
 * document of the page's heap and its first <svg> is drawn as an inline one.
 *
 * Returns 0, ENOTSUP for bytes that are not SVG, or an errno value.
 */
int
page_svg_decode(
	struct page *page,
	const unsigned char *bytes,
	size_t length,
	struct img_bitmap *bitmap)
{
	struct dom_document *document;
	struct html_parser *parser;
	struct dom_element *svg;
	struct vm_cell *root;
	struct wb_units units;
	size_t start;
	int error;

	/* The text must hold an <svg> start tag near its start (after a BOM, whitespace, an XML declaration or comments). */
	start = 0;
	if (length >= 3U && bytes[0] == 0xefU && bytes[1] == 0xbbU && bytes[2] == 0xbfU)
		start = 3U;
	if (length - start < 5U || memmem(bytes + start, length - start < 2048U ? length - start : 2048U, "<svg", 4) == NULL)
		return ENOTSUP;

	/* The text as UTF-16 into a scratch document, which the heap's roots keep while it is parsed. */
	wb_units_init(&units);
	error = wb_utf8_to_units(bytes + start, length - start, &units);
	if (error != 0) {
		wb_units_release(&units);
		return error;
	}
	document = dom_document_create(page->heap);
	if (document == NULL) {
		wb_units_release(&units);
		return ENOMEM;
	}
	root = &document->node.cell;
	error = vm_heap_add_root(page->heap, &root);
	if (error != 0) {
		wb_units_release(&units);
		return error;
	}
	error = html_parser_create(&parser, document, 0);
	if (error == 0) {
		error = html_parser_feed(parser, units.data, units.length);
		if (error == 0)
			error = html_parser_finish(parser);
		html_parser_destroy(parser);
	}
	wb_units_release(&units);

	/* The first svg element of the document. */
	svg = NULL;
	if (error == 0)
		svg = svg_find_root(&document->node, 0);
	if (error == 0 && svg == NULL)
		error = ENOTSUP;
	if (error == 0)
		error = page_svg_render(svg, bitmap);
	vm_heap_remove_root(page->heap, &root);
	return error;
}

/* Finds the first svg element of a tree (depth first). */
static struct dom_element *
svg_find_root(
	struct dom_node *node,
	int depth)
{
	struct dom_element *found;
	struct dom_node *child;

	if (depth > SVG_DEPTH)
		return NULL;
	if (node->type == DOM_ELEMENT && ((struct dom_element *)node)->ns == DOM_NS_SVG &&
	    ((struct dom_element *)node)->tag == DOM_TAG_SVG)
		return (struct dom_element *)node;
	for (child = node->first_child; child != NULL; child = child->next) {
		found = svg_find_root(child, depth + 1);
		if (found != NULL)
			return found;
	}
	return NULL;
}

/* Adds a polygon of points to a shape, oriented so every polygon of a stroke winds the same way. */
static int
svg_stroke_polygon(
	struct svg_shape *shape,
	const float *xs,
	const float *ys,
	int count)
{
	struct svg_point *grown;
	float area;
	int index;
	int source;
	size_t capacity;

	area = 0.0f;
	for (index = 0; index < count; index++) {
		source = (index + 1) % count;
		area += xs[index] * ys[source] - xs[source] * ys[index];
	}
	if (shape->count + (size_t)count > SVG_POINTS_MAX)
		return 0;
	while (shape->count + (size_t)count > shape->capacity) {
		capacity = shape->capacity == 0U ? 256U : shape->capacity * 2U;
		grown = realloc(shape->points, capacity * sizeof(*grown));
		if (grown == NULL)
			return ENOMEM;
		shape->points = grown;
		shape->capacity = capacity;
	}
	for (index = 0; index < count; index++) {
		source = area >= 0.0f ? index : count - 1 - index;
		shape->points[shape->count].x = xs[source];
		shape->points[shape->count].y = ys[source];
		shape->points[shape->count].begin = index == 0;
		shape->points[shape->count].closed = 1;
		shape->count++;
	}
	return 0;
}

/* Adds a disc (a round join or cap) of a radius at a point. */
static int
svg_stroke_disc(
	struct svg_shape *out,
	float x,
	float y,
	float radius)
{
	float xs[16];
	float ys[16];
	int index;

	for (index = 0; index < 16; index++) {
		xs[index] = x + radius * (float)cos((double)index * 3.14159265358979 / 8.0);
		ys[index] = y + radius * (float)sin((double)index * 3.14159265358979 / 8.0);
	}
	return svg_stroke_polygon(out, xs, ys, 16);
}

/*
 * Strokes a shape: each segment becomes a rectangle of the stroke's width, each
 * joint and round cap a disc (joins are round; miter and bevel corners are
 * not told apart), all filled together as one nonzero shape.  Dashes are not
 * drawn.
 */
static int
svg_stroke(
	struct svg_canvas *canvas,
	const struct svg_shape *shape,
	const struct svg_matrix *matrix,
	const struct svg_style *style,
	uint32_t color,
	float alpha)
{
	struct svg_shape outline;
	float xs[4];
	float ys[4];
	float half;
	float scale;
	float dx;
	float dy;
	float length;
	float nx;
	float ny;
	float ex0;
	float ey0;
	float ex1;
	float ey1;
	size_t first;
	size_t last;
	size_t point;
	size_t next;
	size_t segments;
	int closed;
	int error;

	/* The width in device pixels, by the transform's scale. */
	scale = (float)sqrt((double)fabsf(matrix->a * matrix->d - matrix->b * matrix->c));
	half = style->stroke_width * scale / 2.0f;
	if (half <= 0.0f)
		return 0;
	if (half < 0.25f)
		half = 0.25f;
	memset(&outline, 0, sizeof(outline));
	error = 0;
	first = 0;
	while (first < shape->count && error == 0) {
		/* One subpath. */
		last = first + 1U;
		while (last < shape->count && !shape->points[last].begin)
			last++;
		closed = shape->points[first].closed;
		segments = last - first;
		if (!closed)
			segments--;
		for (point = 0; point < segments && error == 0; point++) {
			next = first + (point + 1U) % (last - first);
			ex0 = shape->points[first + point].x;
			ey0 = shape->points[first + point].y;
			ex1 = shape->points[next].x;
			ey1 = shape->points[next].y;
			dx = ex1 - ex0;
			dy = ey1 - ey0;
			length = (float)sqrt((double)(dx * dx + dy * dy));
			if (length < 0.0001f)
				continue;
			dx /= length;
			dy /= length;
			nx = -dy * half;
			ny = dx * half;

			/* Square caps extend an open path's ends. */
			if (!closed && style->stroke_cap == 2) {
				if (point == 0) {
					ex0 -= dx * half;
					ey0 -= dy * half;
				}
				if (point + 1U == segments) {
					ex1 += dx * half;
					ey1 += dy * half;
				}
			}
			xs[0] = ex0 + nx;
			ys[0] = ey0 + ny;
			xs[1] = ex1 + nx;
			ys[1] = ey1 + ny;
			xs[2] = ex1 - nx;
			ys[2] = ey1 - ny;
			xs[3] = ex0 - nx;
			ys[3] = ey0 - ny;
			error = svg_stroke_polygon(&outline, xs, ys, 4);

			/* A round joint at the segment's end (and the first point of an open path with round caps). */
			if (error == 0 && half > 0.6f && (closed || point + 1U < segments || style->stroke_cap == 1))
				error = svg_stroke_disc(&outline, shape->points[next].x, shape->points[next].y, half);
			if (error == 0 && half > 0.6f && point == 0 && !closed && style->stroke_cap == 1)
				error = svg_stroke_disc(&outline, shape->points[first].x, shape->points[first].y, half);
		}
		first = last;
	}
	if (error == 0 && outline.count >= 3U)
		error = svg_fill(canvas, &outline, color, alpha, SVG_NONZERO);
	free(outline.points);
	return error;
}

/* Draws one node: a group passes its matrix and style on to its children; a shape is drawn. */
static int
svg_draw_node(
	struct svg_canvas *canvas,
	const struct dom_element *element,
	const struct svg_matrix *matrix,
	const struct svg_style *style,
	int depth)
{
	if (svg_name_is(element, "g") || svg_name_is(element, "a") || svg_name_is(element, "svg") || svg_name_is(element, "switch"))
		return svg_walk(canvas, &element->node, matrix, style, depth + 1);
	if (svg_name_is(element, "use"))
		return svg_draw_use(canvas, element, matrix, style, depth);
	return svg_draw_element(canvas, element, matrix, style);
}

/* Finds the clipPath element an element's clip-path names (url(#id)); NULL for none. */
static const struct dom_element *
svg_clip_path_of(
	const struct dom_element *element)
{
	struct wb_buffer buffer;
	char reference[128];
	const struct dom_node *top;
	const struct dom_element *found;
	const char *text;
	const char *start;
	const char *style_text;
	size_t length;

	/* The attribute, or the property in the style attribute. */
	found = NULL;
	wb_buffer_init(&buffer);
	text = svg_attribute(element, "clip-path", &buffer);
	if (text == NULL) {
		wb_buffer_release(&buffer);
		wb_buffer_init(&buffer);
		style_text = svg_attribute(element, "style", &buffer);
		text = NULL;
		if (style_text != NULL)
			text = strstr(style_text, "clip-path");
		if (text != NULL) {
			text = strchr(text, ':');
			if (text != NULL)
				text++;
		}
	}
	if (text == NULL) {
		wb_buffer_release(&buffer);
		return NULL;
	}
	while (*text == ' ')
		text++;
	if (strncmp(text, "url(", 4) != 0) {
		wb_buffer_release(&buffer);
		return NULL;
	}
	start = strchr(text, '#');
	if (start == NULL) {
		wb_buffer_release(&buffer);
		return NULL;
	}
	start++;
	length = 0;
	while (start[length] != '\0' && start[length] != ')' && start[length] != '\'' && start[length] != '"' && length + 1U < sizeof(reference))
		length++;
	memcpy(reference, start, length);
	reference[length] = '\0';
	wb_buffer_release(&buffer);

	/* The element of that id under the outermost svg. */
	top = &element->node;
	while (top->parent != NULL && top->parent->type == DOM_ELEMENT)
		top = top->parent;
	found = svg_find_id(top, reference, 0);
	if (found != NULL && !svg_name_is(found, "clipPath"))
		return NULL;
	return found;
}

/* Allocates a transparent canvas of the size of another. */
static int
svg_canvas_like(
	struct svg_canvas *canvas,
	const struct svg_canvas *model)
{
	canvas->width = model->width;
	canvas->height = model->height;
	canvas->pixels = calloc((size_t)model->width * (size_t)model->height * 4U, sizeof(float));
	canvas->coverage = calloc((size_t)model->width + 2U, sizeof(float));
	if (canvas->pixels == NULL || canvas->coverage == NULL) {
		free(canvas->pixels);
		free(canvas->coverage);
		return ENOMEM;
	}
	return 0;
}

/*
 * Draws an element through a clip path: the element is drawn on a canvas of
 * its own, the clip path's shapes (filled, nonzero, in the element's user
 * space) make a mask on another, and the first goes onto the canvas where
 * the second covers.
 */
static int
svg_draw_clipped(
	struct svg_canvas *canvas,
	const struct dom_element *element,
	const struct svg_matrix *matrix,
	const struct svg_style *style,
	const struct dom_element *clip,
	int depth)
{
	struct svg_canvas layer;
	struct svg_canvas mask;
	struct svg_style plain;
	size_t pixel;
	size_t count;
	float cover;
	float alpha;
	int channel;
	int error;

	error = svg_canvas_like(&layer, canvas);
	if (error != 0)
		return error;
	error = svg_canvas_like(&mask, canvas);
	if (error != 0) {
		free(layer.pixels);
		free(layer.coverage);
		return error;
	}
	error = svg_draw_node(&layer, element, matrix, style, depth);
	if (error == 0) {
		memset(&plain, 0, sizeof(plain));
		plain.fill = 0xffffffffU;
		plain.fill_opacity = 1.0f;
		plain.opacity = 1.0f;
		plain.rule = SVG_NONZERO;
		plain.stroke_none = 1;
		plain.stroke_width = 1.0f;
		plain.stroke_opacity = 1.0f;
		error = svg_walk(&mask, &clip->node, matrix, &plain, depth + 1);
	}
	if (error == 0) {
		count = (size_t)canvas->width * (size_t)canvas->height;
		for (pixel = 0; pixel < count; pixel++) {
			cover = mask.pixels[pixel * 4U + 3U];
			if (cover <= 0.0f)
				continue;
			if (cover > 1.0f)
				cover = 1.0f;
			alpha = layer.pixels[pixel * 4U + 3U];
			for (channel = 0; channel < 4; channel++)
				canvas->pixels[pixel * 4U + (size_t)channel] = layer.pixels[pixel * 4U + (size_t)channel] * cover +
				    canvas->pixels[pixel * 4U + (size_t)channel] * (1.0f - alpha * cover);
		}
	}
	free(layer.pixels);
	free(layer.coverage);
	free(mask.pixels);
	free(mask.coverage);
	return error;
}

/* Reads an element's href (xlink:href or href) as UTF-8 text in the buffer; NULL when it has none. */
static const char *
svg_href(
	const struct dom_element *element,
	struct wb_buffer *buffer)
{
	size_t index;

	for (index = 0; index < element->attribute_count; index++) {
		if (!svg_same_name(element->attributes[index].name, "href"))
			continue;
		if (vm_string_to_utf8(element->attributes[index].value, buffer) != 0)
			return NULL;
		return wb_buffer_string(buffer);
	}
	return NULL;
}

/* Draws a <use>: the element its href names (a #id of this svg), moved by x and y, under the use's style. */
static int
svg_draw_use(
	struct svg_canvas *canvas,
	const struct dom_element *element,
	const struct svg_matrix *matrix,
	const struct svg_style *style,
	int depth)
{
	struct svg_matrix moved;
	struct svg_matrix shift;
	struct svg_matrix local;
	struct svg_matrix placed;
	struct svg_style own;
	struct wb_buffer buffer;
	const struct dom_element *target;
	const struct dom_node *top;
	const char *text;
	float x;
	float y;
	int hidden;
	int error;

	/* The target by its id, under the outermost svg. */
	wb_buffer_init(&buffer);
	text = svg_href(element, &buffer);
	if (text == NULL || text[0] != '#' || depth > SVG_DEPTH) {
		wb_buffer_release(&buffer);
		return 0;
	}
	top = &element->node;
	while (top->parent != NULL && top->parent->type == DOM_ELEMENT)
		top = top->parent;
	target = svg_find_id(top, text + 1, 0);
	wb_buffer_release(&buffer);
	if (target == NULL || target == element)
		return 0;

	/* The use's x and y move the target. */
	x = svg_parse_attribute_number(element, "x", 0.0f);
	y = svg_parse_attribute_number(element, "y", 0.0f);
	shift.a = 1.0f;
	shift.b = 0.0f;
	shift.c = 0.0f;
	shift.d = 1.0f;
	shift.e = x;
	shift.f = y;
	svg_matrix_multiply(&moved, matrix, &shift);

	/* The target's own transform and style, over the use's. */
	placed = moved;
	wb_buffer_init(&buffer);
	text = svg_attribute(target, "transform", &buffer);
	if (text != NULL && svg_matrix_parse(text, &local))
		svg_matrix_multiply(&placed, &moved, &local);
	wb_buffer_release(&buffer);
	own = *style;
	hidden = 0;
	svg_style_read(target, &own, &hidden);
	if (hidden)
		return 0;
	error = svg_draw_node(canvas, target, &placed, &own, depth + 1);
	return error;
}
