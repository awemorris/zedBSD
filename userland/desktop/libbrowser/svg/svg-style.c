/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The presentation properties of SVG elements.
 */

#include "svg/svg-style.h"

#include <errno.h>
#include <float.h>
#include <string.h>

/*
 * The longest declaration (or presentation attribute value) that is read.
 * The text of one declaration is copied into a buffer of this size with
 * its comments taken out; a longer one is dropped like an invalid one.
 * The longest value any property takes is a dash array of
 * VEC_DASHES_MAX numbers, which is far shorter.
 */
#define STYLE_TEXT_MAX			4096

/* The font size every element starts with, the initial value of font-size (CSS px). */
#define STYLE_FONT_SIZE_INITIAL		16.0f

/*
 * The size rem is taken against.  The document root's font size is not in
 * the fragment, so the initial size, which the root has unless a style
 * sheet of the page changes it, stands for it.
 */
#define STYLE_FONT_SIZE_ROOT		16.0f

/* The most elements svg_style_of keeps while it climbs: the root and SVG_STYLE_DEPTH_MAX below it. */
#define STYLE_CHAIN_MAX			(SVG_STYLE_DEPTH_MAX + 1)

/* The straight 0xAARRGGBB of black, the initial color, fill and stop-color. */
#define STYLE_BLACK			0xff000000U

/* The properties this module knows, in the order of the table below. */
enum style_property {
	STYLE_FILL = 0,
	STYLE_FILL_RULE,
	STYLE_FILL_OPACITY,
	STYLE_STROKE,
	STYLE_STROKE_WIDTH,
	STYLE_STROKE_OPACITY,
	STYLE_STROKE_LINECAP,
	STYLE_STROKE_LINEJOIN,
	STYLE_STROKE_MITERLIMIT,
	STYLE_STROKE_DASHARRAY,
	STYLE_STROKE_DASHOFFSET,
	STYLE_COLOR,
	STYLE_VISIBILITY,
	STYLE_CLIP_RULE,
	STYLE_FONT_SIZE,
	STYLE_DISPLAY,
	STYLE_OPACITY,
	STYLE_CLIP_PATH,
	STYLE_STOP_COLOR,
	STYLE_STOP_OPACITY
};

/* The CSS-wide keywords a value may be: inherit, initial and unset. */
enum style_wide {
	STYLE_WIDE_NONE = 0,
	STYLE_WIDE_INHERIT,
	STYLE_WIDE_INITIAL,
	STYLE_WIDE_UNSET
};

/* One property: its name (which is also its presentation attribute's name), what it is, and whether it inherits. */
struct style_property_entry {
	const char *name;
	enum style_property property;
	int inherited;
};

/* One keyword of a property that takes keywords, and the value it stands for. */
struct style_keyword {
	const char *name;
	int value;
};

/*
 * A buffer that collects the text of one declaration or one value, with
 * the room it has left and whether the text did not fit.
 */
struct style_text {
	char *buffer;
	size_t size;
	size_t length;
	int overflow;
};

/*
 * The properties, as the property definitions give them.
 *
 * Values referenced: SVG 2 §13.4.1, §13.4.2, §13.4.3, §13.5.1 to §13.5.6
 * (fill, fill-rule, fill-opacity, stroke, stroke-opacity, stroke-width,
 * stroke-linecap, stroke-linejoin, stroke-miterlimit, stroke-dasharray,
 * stroke-dashoffset: all inherited), §13.3 (color: inherited),
 * §3.2.3 (display: not inherited; visibility: inherited), §3.6.1
 * (opacity: not inherited), §14.2.4.2 (stop-color and stop-opacity: not
 * inherited), CSS Masking 1 §5.1 (clip-path: not inherited) and §6.2
 * (clip-rule: inherited), CSS Fonts (font-size: inherited).  SVG 2 §6.6
 * names the presentation attribute of each by the property's name.
 */
static const struct style_property_entry style_properties[] = {
	{ "fill", STYLE_FILL, 1 },
	{ "fill-rule", STYLE_FILL_RULE, 1 },
	{ "fill-opacity", STYLE_FILL_OPACITY, 1 },
	{ "stroke", STYLE_STROKE, 1 },
	{ "stroke-width", STYLE_STROKE_WIDTH, 1 },
	{ "stroke-opacity", STYLE_STROKE_OPACITY, 1 },
	{ "stroke-linecap", STYLE_STROKE_LINECAP, 1 },
	{ "stroke-linejoin", STYLE_STROKE_LINEJOIN, 1 },
	{ "stroke-miterlimit", STYLE_STROKE_MITERLIMIT, 1 },
	{ "stroke-dasharray", STYLE_STROKE_DASHARRAY, 1 },
	{ "stroke-dashoffset", STYLE_STROKE_DASHOFFSET, 1 },
	{ "color", STYLE_COLOR, 1 },
	{ "visibility", STYLE_VISIBILITY, 1 },
	{ "clip-rule", STYLE_CLIP_RULE, 1 },
	{ "font-size", STYLE_FONT_SIZE, 1 },
	{ "display", STYLE_DISPLAY, 0 },
	{ "opacity", STYLE_OPACITY, 0 },
	{ "clip-path", STYLE_CLIP_PATH, 0 },
	{ "stop-color", STYLE_STOP_COLOR, 0 },
	{ "stop-opacity", STYLE_STOP_OPACITY, 0 }
};

/*
 * The winding rules of fill-rule and clip-rule.
 *
 * Values referenced: SVG 2 §13.4.2 (nonzero | evenodd), CSS Masking 1
 * §6.2 (nonzero | evenodd).
 */
static const struct style_keyword style_rules[] = {
	{ "nonzero", VEC_FILL_NONZERO },
	{ "evenodd", VEC_FILL_EVENODD }
};

/*
 * The values of stroke-linecap.
 *
 * Values referenced: SVG 2 §13.5.4 (butt | round | square).
 */
static const struct style_keyword style_caps[] = {
	{ "butt", VEC_CAP_BUTT },
	{ "round", VEC_CAP_ROUND },
	{ "square", VEC_CAP_SQUARE }
};

/*
 * The values of stroke-linejoin.  miter-clip and arcs, which the published
 * SVG 2 names besides these, are drawn as miter (the stroke style has no
 * value for them).
 *
 * Values referenced: SVG 2 §13.5.5 (miter | round | bevel), and the
 * miter-clip and arcs of the SVG 2 Candidate Recommendation, whose
 * fallback is miter.
 */
static const struct style_keyword style_joins[] = {
	{ "miter", VEC_JOIN_MITER },
	{ "miter-clip", VEC_JOIN_MITER },
	{ "arcs", VEC_JOIN_MITER },
	{ "round", VEC_JOIN_ROUND },
	{ "bevel", VEC_JOIN_BEVEL }
};

/*
 * The values of visibility: collapse paints nothing, like hidden.
 *
 * Values referenced: SVG 2 §3.2.3 (visible, hidden and collapse), CSS
 * Box Model's visibility.
 */
static const struct style_keyword style_visibilities[] = {
	{ "visible", SVG_VISIBILITY_VISIBLE },
	{ "hidden", SVG_VISIBILITY_HIDDEN },
	{ "collapse", SVG_VISIBILITY_HIDDEN }
};

/*
 * The values of display.  none hides the element; every other value is
 * rendered as normal (SVG 2 §3.2.3), so they all stand for inline here.
 * The two-keyword forms (block flow) are not read.
 *
 * Values referenced: SVG 2 §3.2.3, CSS Display 3 §2 (the display
 * keywords: the box types, list-item, the internal table and ruby
 * types, contents and none).
 */
static const struct style_keyword style_displays[] = {
	{ "none", SVG_DISPLAY_NONE },
	{ "inline", SVG_DISPLAY_INLINE },
	{ "block", SVG_DISPLAY_INLINE },
	{ "run-in", SVG_DISPLAY_INLINE },
	{ "flow", SVG_DISPLAY_INLINE },
	{ "flow-root", SVG_DISPLAY_INLINE },
	{ "list-item", SVG_DISPLAY_INLINE },
	{ "inline-block", SVG_DISPLAY_INLINE },
	{ "table", SVG_DISPLAY_INLINE },
	{ "inline-table", SVG_DISPLAY_INLINE },
	{ "table-row-group", SVG_DISPLAY_INLINE },
	{ "table-header-group", SVG_DISPLAY_INLINE },
	{ "table-footer-group", SVG_DISPLAY_INLINE },
	{ "table-row", SVG_DISPLAY_INLINE },
	{ "table-cell", SVG_DISPLAY_INLINE },
	{ "table-column-group", SVG_DISPLAY_INLINE },
	{ "table-column", SVG_DISPLAY_INLINE },
	{ "table-caption", SVG_DISPLAY_INLINE },
	{ "flex", SVG_DISPLAY_INLINE },
	{ "inline-flex", SVG_DISPLAY_INLINE },
	{ "grid", SVG_DISPLAY_INLINE },
	{ "inline-grid", SVG_DISPLAY_INLINE },
	{ "ruby", SVG_DISPLAY_INLINE },
	{ "ruby-base", SVG_DISPLAY_INLINE },
	{ "ruby-text", SVG_DISPLAY_INLINE },
	{ "ruby-base-container", SVG_DISPLAY_INLINE },
	{ "ruby-text-container", SVG_DISPLAY_INLINE },
	{ "contents", SVG_DISPLAY_INLINE }
};

/*
 * The elements whose 'fill' attribute is not a presentation attribute.
 *
 * Values referenced: SVG 2 §6.6 (fill is a presentation attribute of any
 * element in the SVG namespace except the animation elements, which have
 * a different fill attribute); the animation elements are animate,
 * animateMotion, animateTransform and set (SVG Animations).
 */
static const char *const style_animation_elements[] = {
	"animate",
	"animateMotion",
	"animateTransform",
	"set"
};

/* Forward declarations of the static functions. */
static int style_is_space(char c);
static char style_lower(char c);
static const char *style_skip_space(const char *p);
static int style_single_word(const char *text, const char **word, size_t *length);
static int style_word_equal(const char *text, size_t length, const char *keyword);
static const struct style_property_entry *style_find_property(const char *name, size_t length);
static int style_is_animation_element(const char *name);
static enum style_wide style_wide_keyword(const char *value);
static void style_copy_property(struct svg_style *style, const struct svg_style *source, enum style_property property);
static void style_declare_wide(struct svg_style *style, const struct svg_style *parent, const struct style_property_entry *entry, enum style_wide wide);
static int style_declare_value(struct svg_style *style, const struct svg_style *parent, const struct svg_tree *tree, const struct style_property_entry *entry, const char *value, int attribute);
static int style_parse_keyword(const char *value, const struct style_keyword *table, size_t count, int *result);
static int style_parse_opacity(const char *value, float *opacity);
static int style_parse_length(const char *value, int non_negative, struct svg_length *length);
static int style_parse_dasharray(const char *value, struct svg_style *style);
static int style_parse_miterlimit(const char *value, float *limit);
static int style_parse_color(const struct svg_tree *tree, const char *value, uint32_t *color, int *current);
static int style_parse_font_size(const char *value, float parent_size, int attribute, float *size);
static int style_parse_clip_path(const char *value, struct svg_style *style);
static void style_text_put(struct style_text *text, char c);
static int style_read_text(const char **cursor, int stop_at_semicolon, char *buffer, size_t size);
static void style_strip_important(char *value);
static void style_apply_declaration(struct svg_style *style, const struct svg_style *parent, const struct svg_tree *tree, char *text);

/*
 * Sets every property to its initial value.
 *
 * Values referenced: the Initial row of each property definition (the
 * table of style_properties lists the sections).  Lengths stay as the
 * definitions write them: stroke-width 1 and stroke-dashoffset 0 in user
 * units, which is what px is in the initial viewport.
 */
void
svg_style_initial(
	struct svg_style *style)
{
	/* Refuses a missing style. */
	if (style == NULL)
		return;

	/* Clears everything, so that every field the list below leaves out (the dash lengths, the id) is 0. */
	memset(style, 0, sizeof(*style));

	/* Sets the fill: black. */
	style->fill.kind = SVG_PAINT_COLOR;
	style->fill.color = STYLE_BLACK;
	style->fill.fallback_kind = SVG_PAINT_NONE;
	style->fill_rule = VEC_FILL_NONZERO;
	style->fill_opacity = 1.0f;

	/* Sets the stroke: none, one unit wide, butt caps, miter joins with a limit of 4, no dashes. */
	style->stroke.kind = SVG_PAINT_NONE;
	style->stroke.fallback_kind = SVG_PAINT_NONE;
	style->stroke_width.value = 1.0f;
	style->stroke_width.unit = SVG_UNIT_NUMBER;
	style->stroke_opacity = 1.0f;
	style->stroke_linecap = VEC_CAP_BUTT;
	style->stroke_linejoin = VEC_JOIN_MITER;
	style->stroke_miterlimit = 4.0f;
	style->dash_count = 0;
	style->dash_offset.value = 0.0f;
	style->dash_offset.unit = SVG_UNIT_NUMBER;

	/* Sets the properties that decide whether and how the element is drawn at all. */
	style->color = STYLE_BLACK;
	style->visibility = SVG_VISIBILITY_VISIBLE;
	style->clip_rule = VEC_FILL_NONZERO;
	style->font_size = STYLE_FONT_SIZE_INITIAL;
	style->display = SVG_DISPLAY_INLINE;
	style->opacity = 1.0f;
	style->has_clip_path = 0;

	/* Sets the gradient stop's properties. */
	style->stop_color = STYLE_BLACK;
	style->stop_color_current = 0;
	style->stop_opacity = 1.0f;
}

/*
 * Makes a child's starting style from its parent's.
 *
 * The result is built apart and then stored, so the child may be the
 * parent itself.  Values referenced: the Inherited row of each property
 * definition (see style_properties).
 */
void
svg_style_inherit(
	struct svg_style *child,
	const struct svg_style *parent)
{
	struct svg_style result;

	/* Refuses a missing child. */
	if (child == NULL)
		return;

	/* Starts from the initial values, which the properties that do not inherit keep. */
	svg_style_initial(&result);

	/* Copies the properties that inherit. */
	if (parent != NULL) {
		result.fill = parent->fill;
		result.fill_rule = parent->fill_rule;
		result.fill_opacity = parent->fill_opacity;
		result.stroke = parent->stroke;
		result.stroke_width = parent->stroke_width;
		result.stroke_opacity = parent->stroke_opacity;
		result.stroke_linecap = parent->stroke_linecap;
		result.stroke_linejoin = parent->stroke_linejoin;
		result.stroke_miterlimit = parent->stroke_miterlimit;
		memcpy(result.dashes, parent->dashes, sizeof(result.dashes));
		result.dash_count = parent->dash_count;
		result.dash_offset = parent->dash_offset;
		result.color = parent->color;
		result.visibility = parent->visibility;
		result.clip_rule = parent->clip_rule;
		result.font_size = parent->font_size;
	}

	/* Stores the result. */
	*child = result;
}

/*
 * Applies one declaration to a style.
 *
 * The text is a declaration's value: the grammar of the property (CSS
 * values), so a bare number is a length only where the property's
 * grammar says (stroke-width, stroke-dashoffset and stroke-dasharray take
 * numbers; font-size takes only a zero), unlike the presentation
 * attribute of svg_style_apply.  Comments in the value are skipped; a
 * value longer than the buffer of one declaration is invalid.
 */
int
svg_style_declare(
	struct svg_style *style,
	const struct svg_style *parent,
	const struct svg_tree *tree,
	const char *name,
	const char *value)
{
	const struct style_property_entry *entry;
	char text[STYLE_TEXT_MAX];
	const char *cursor;
	int error;

	/* Refuses missing arguments. */
	if (style == NULL || name == NULL || value == NULL)
		return EINVAL;

	/* Ignores a property this module does not know. */
	entry = style_find_property(name, strlen(name));
	if (entry == NULL)
		return 0;

	/* Copies the value without its comments. */
	cursor = value;
	error = style_read_text(&cursor, 0, text, sizeof(text));
	if (error != 0)
		return EINVAL;

	/* Reads the value in the grammar of a declaration. */
	error = style_declare_value(style, parent, tree, entry, text, 0);
	if (error != 0)
		return error;

	/* Succeeded: the property has the value. */
	return 0;
}

/*
 * Applies an element's presentation attributes, then its style attribute.
 *
 * Values referenced: SVG 2 §6.6 (presentation attributes are parsed as the
 * property's CSS value, with a bare number also accepted where a length
 * is, SVG 2 §4.2; a presentation attribute is author-level of
 * specificity 0, so every declaration of the style attribute overrides
 * them; an !important in a presentation attribute makes it invalid),
 * CSS Style Attributes (the style attribute is a declaration list).
 * Nothing is applied to an element that is not in the SVG namespace.
 */
int
svg_style_apply(
	struct svg_style *style,
	const struct svg_style *parent,
	const struct svg_tree *tree,
	const struct svg_node *element)
{
	const struct style_property_entry *entry;
	const char *element_name;
	const char *attribute;
	const char *cursor;
	char text[STYLE_TEXT_MAX];
	size_t index;
	int animation;
	int error;

	/* Refuses missing arguments. */
	if (style == NULL || tree == NULL || element == NULL)
		return EINVAL;

	/* Leaves an element of another namespace alone: only SVG elements have presentation attributes. */
	element_name = tree->name(tree->context, element);
	if (element_name == NULL)
		return 0;

	/* Tells whether the 'fill' attribute is the animation elements' own attribute. */
	animation = style_is_animation_element(element_name);

	/* Applies the presentation attributes, each as its property's value. */
	for (index = 0;
	     index < sizeof(style_properties) / sizeof(style_properties[0]);
	     index++) {
		entry = &style_properties[index];
		if (entry->property == STYLE_FILL && animation)
			continue;
		attribute = tree->attribute(tree->context, element, entry->name);
		if (attribute == NULL)
			continue;

		/* Reads the value without its comments; one that does not fit is invalid. */
		cursor = attribute;
		error = style_read_text(&cursor, 0, text, sizeof(text));
		if (error != 0)
			continue;

		/* An invalid value is dropped: the property keeps what it had. */
		(void)style_declare_value(style, parent, tree, entry, text, 1);
	}

	/* Reads the style attribute's declarations. */
	attribute = tree->attribute(tree->context, element, "style");
	if (attribute != NULL) {
		cursor = attribute;
		while (*cursor != '\0') {
			/* Copies one declaration, up to the semicolon that ends it. */
			error = style_read_text(&cursor, 1, text, sizeof(text));
			if (error != 0)
				continue;

			/* Applies it, if it is one of ours and well formed. */
			style_apply_declaration(style, parent, tree, text);
		}
	}

	/* Succeeded: the values that were invalid are dropped. */
	return 0;
}

/*
 * Computes the style of any element of the SVG fragment from its ancestors.
 *
 * The chain from the element up to the outermost SVG ancestor is
 * collected first, then the styles are made from the top down as the
 * walk of the rendering tree makes them.
 */
int
svg_style_of(
	const struct svg_tree *tree,
	const struct svg_node *element,
	const struct svg_style *outer,
	struct svg_style *style)
{
	const struct svg_node *chain[STYLE_CHAIN_MAX];
	const struct svg_node *node;
	const struct svg_node *parent_node;
	const char *parent_name;
	struct svg_style current;
	struct svg_style next;
	size_t count;
	size_t index;
	int error;

	/* Refuses a missing output. */
	if (style == NULL)
		return EINVAL;

	/* Leaves the initial style where the element cannot be followed. */
	svg_style_initial(style);

	/* Refuses missing arguments. */
	if (tree == NULL || element == NULL)
		return EINVAL;

	/* Climbs from the element to the outermost ancestor that is an SVG element. */
	count = 0;
	node = element;
	while (node != NULL) {
		/* Refuses an element deeper than the chain holds. */
		if (count >= STYLE_CHAIN_MAX)
			return ENOBUFS;

		chain[count] = node;
		count++;

		/* Goes up while the parent is an element of the SVG namespace. */
		parent_node = tree->parent(tree->context, node);
		node = NULL;
		if (parent_node != NULL) {
			parent_name = tree->name(tree->context, parent_node);
			if (parent_name != NULL)
				node = parent_node;
		}
	}

	/* Makes the root's style from the style outside the fragment. */
	svg_style_inherit(&current, outer);
	error = svg_style_apply(&current, outer, tree, chain[count - 1]);
	if (error != 0)
		return error;

	/* Makes each element's style from its parent's, down to the element. */
	for (index = count - 1;
	     index > 0;
	     index--) {
		svg_style_inherit(&next, &current);
		error = svg_style_apply(&next, &current, tree, chain[index - 1]);
		if (error != 0)
			return error;
		current = next;
	}

	/* Publishes the element's style. */
	*style = current;

	/* Succeeded: *style is the element's style. */
	return 0;
}

/* Tells whether a character is one of the white space characters CSS allows. */
static int
style_is_space(
	char c)
{
	/* Space, tab, line feed, form feed and carriage return are white space. */
	if (c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r')
		return 1;

	/* Succeeded: anything else is not white space. */
	return 0;
}

/* Gives the ASCII lower case of a character, which other characters keep. */
static char
style_lower(
	char c)
{
	/* Moves an upper case letter to its lower case. */
	if (c >= 'A' && c <= 'Z')
		return (char)(c - 'A' + 'a');

	/* Succeeded: the character is not an upper case letter. */
	return c;
}

/* Skips the white space at a position, stopping at the first other character or the NUL. */
static const char *
style_skip_space(
	const char *p)
{
	int space;

	/* Moves past every white space character. */
	space = style_is_space(*p);
	while (space) {
		p++;
		space = style_is_space(*p);
	}

	/* Reports the position of the first character that is not white space. */
	return p;
}

/*
 * Tells whether a text is one word with white space around it, and where
 * the word is.  A word is a run of characters that are not white space.
 */
static int
style_single_word(
	const char *text,
	const char **word,
	size_t *length)
{
	const char *start;
	const char *end;
	const char *rest;
	int space;

	/* Finds the word after the leading white space. */
	start = style_skip_space(text);
	end = start;
	space = style_is_space(*end);
	while (*end != '\0' && !space) {
		end++;
		space = style_is_space(*end);
	}

	/* Refuses an empty text. */
	if (end == start)
		return 0;

	/* Refuses anything but white space after the word. */
	rest = style_skip_space(end);
	if (*rest != '\0')
		return 0;

	/* Reports the word. */
	*word = start;
	*length = (size_t)(end - start);

	/* Succeeded: the text is one word. */
	return 1;
}

/*
 * Tells whether the length bytes at text are the keyword, which is given in
 * lower case, without regard to the ASCII case of the text (CSS keywords
 * are matched so).  The text is never read beyond length bytes.
 */
static int
style_word_equal(
	const char *text,
	size_t length,
	const char *keyword)
{
	size_t index;

	/* Refuses a word of another length. */
	if (strlen(keyword) != length)
		return 0;

	/* Compares the letters one by one, stopping at the first difference. */
	for (index = 0; index < length; index++) {
		if (style_lower(text[index]) != keyword[index])
			return 0;
	}

	/* Succeeded: the word is the keyword. */
	return 1;
}

/* Finds a property by its name (ASCII case-insensitive, as CSS property names are), or NULL. */
static const struct style_property_entry *
style_find_property(
	const char *name,
	size_t length)
{
	size_t index;
	int same;

	/* Looks through the table for the name. */
	for (index = 0;
	     index < sizeof(style_properties) / sizeof(style_properties[0]);
	     index++) {
		same = style_word_equal(name, length, style_properties[index].name);
		if (same)
			return &style_properties[index];
	}

	/* Reports that the property is not one of ours. */
	return NULL;
}

/* Tells whether an element name is one of the animation elements. */
static int
style_is_animation_element(
	const char *name)
{
	size_t index;
	int different;

	/* Looks through the animation elements for the name. */
	for (index = 0;
	     index < sizeof(style_animation_elements) / sizeof(style_animation_elements[0]);
	     index++) {
		different = strcmp(name, style_animation_elements[index]);
		if (different == 0)
			return 1;
	}

	/* Reports that the element is not one of them. */
	return 0;
}

/* Tells whether a value is one of the CSS-wide keywords, and which. */
static enum style_wide
style_wide_keyword(
	const char *value)
{
	const char *word;
	size_t length;
	int found;

	/* Only a value of one word can be a keyword. */
	found = style_single_word(value, &word, &length);
	if (!found)
		return STYLE_WIDE_NONE;

	/* Tells which keyword the word is. */
	found = style_word_equal(word, length, "inherit");
	if (found)
		return STYLE_WIDE_INHERIT;
	found = style_word_equal(word, length, "initial");
	if (found)
		return STYLE_WIDE_INITIAL;
	found = style_word_equal(word, length, "unset");
	if (found)
		return STYLE_WIDE_UNSET;

	/* Reports that the word is another value. */
	return STYLE_WIDE_NONE;
}

/* Copies one property's value from one style to another. */
static void
style_copy_property(
	struct svg_style *style,
	const struct svg_style *source,
	enum style_property property)
{
	/* Copies the fields of the property. */
	switch (property) {
	case STYLE_FILL:
		style->fill = source->fill;
		break;
	case STYLE_FILL_RULE:
		style->fill_rule = source->fill_rule;
		break;
	case STYLE_FILL_OPACITY:
		style->fill_opacity = source->fill_opacity;
		break;
	case STYLE_STROKE:
		style->stroke = source->stroke;
		break;
	case STYLE_STROKE_WIDTH:
		style->stroke_width = source->stroke_width;
		break;
	case STYLE_STROKE_OPACITY:
		style->stroke_opacity = source->stroke_opacity;
		break;
	case STYLE_STROKE_LINECAP:
		style->stroke_linecap = source->stroke_linecap;
		break;
	case STYLE_STROKE_LINEJOIN:
		style->stroke_linejoin = source->stroke_linejoin;
		break;
	case STYLE_STROKE_MITERLIMIT:
		style->stroke_miterlimit = source->stroke_miterlimit;
		break;
	case STYLE_STROKE_DASHARRAY:
		memmove(style->dashes, source->dashes, sizeof(style->dashes));
		style->dash_count = source->dash_count;
		break;
	case STYLE_STROKE_DASHOFFSET:
		style->dash_offset = source->dash_offset;
		break;
	case STYLE_COLOR:
		style->color = source->color;
		break;
	case STYLE_VISIBILITY:
		style->visibility = source->visibility;
		break;
	case STYLE_CLIP_RULE:
		style->clip_rule = source->clip_rule;
		break;
	case STYLE_FONT_SIZE:
		style->font_size = source->font_size;
		break;
	case STYLE_DISPLAY:
		style->display = source->display;
		break;
	case STYLE_OPACITY:
		style->opacity = source->opacity;
		break;
	case STYLE_CLIP_PATH:
		style->has_clip_path = source->has_clip_path;
		memmove(style->clip_path, source->clip_path, sizeof(style->clip_path));
		break;
	case STYLE_STOP_COLOR:
		style->stop_color = source->stop_color;
		style->stop_color_current = source->stop_color_current;
		break;
	case STYLE_STOP_OPACITY:
		style->stop_opacity = source->stop_opacity;
		break;
	}
}

/*
 * Applies a CSS-wide keyword to one property: inherit takes the parent's
 * value, initial the initial one, and unset whichever the property
 * would have without a declaration.
 */
static void
style_declare_wide(
	struct svg_style *style,
	const struct svg_style *parent,
	const struct style_property_entry *entry,
	enum style_wide wide)
{
	struct svg_style initial;
	const struct svg_style *source;
	int from_parent;

	/* Decides whether the value comes from the parent. */
	from_parent = 0;
	if (wide == STYLE_WIDE_INHERIT) {
		from_parent = 1;
	} else if (wide == STYLE_WIDE_UNSET) {
		from_parent = entry->inherited;
	}

	/* Takes the initial values, which also stand for a parent that is not there. */
	svg_style_initial(&initial);
	source = &initial;
	if (from_parent && parent != NULL)
		source = parent;

	/* Copies the property. */
	style_copy_property(style, source, entry->property);
}

/*
 * Applies a value to one property.  attribute tells that the value is a
 * presentation attribute's (a bare number is then also a length).  An
 * invalid value leaves the property as it was.
 */
static int
style_declare_value(
	struct svg_style *style,
	const struct svg_style *parent,
	const struct svg_tree *tree,
	const struct style_property_entry *entry,
	const char *value,
	int attribute)
{
	enum style_wide wide;
	const char *rest;
	uint32_t color;
	uint32_t parent_color;
	float parent_size;
	int current;
	int number;
	int error;

	/* Refuses an empty value. */
	rest = style_skip_space(value);
	if (*rest == '\0')
		return EINVAL;

	/* Applies a CSS-wide keyword. */
	wide = style_wide_keyword(value);
	if (wide != STYLE_WIDE_NONE) {
		style_declare_wide(style, parent, entry, wide);
		return 0;
	}

	/* Reads the value by the property's grammar. */
	error = EINVAL;
	number = 0;
	color = 0;
	current = 0;
	switch (entry->property) {
	case STYLE_FILL:
		error = svg_parse_paint(tree, value, &style->fill);
		break;
	case STYLE_STROKE:
		error = svg_parse_paint(tree, value, &style->stroke);
		break;
	case STYLE_FILL_RULE:
		error = style_parse_keyword(value, style_rules, sizeof(style_rules) / sizeof(style_rules[0]), &number);
		if (error == 0)
			style->fill_rule = (enum vec_fill_rule)number;
		break;
	case STYLE_CLIP_RULE:
		error = style_parse_keyword(value, style_rules, sizeof(style_rules) / sizeof(style_rules[0]), &number);
		if (error == 0)
			style->clip_rule = (enum vec_fill_rule)number;
		break;
	case STYLE_FILL_OPACITY:
		error = style_parse_opacity(value, &style->fill_opacity);
		break;
	case STYLE_STROKE_OPACITY:
		error = style_parse_opacity(value, &style->stroke_opacity);
		break;
	case STYLE_OPACITY:
		error = style_parse_opacity(value, &style->opacity);
		break;
	case STYLE_STOP_OPACITY:
		error = style_parse_opacity(value, &style->stop_opacity);
		break;
	case STYLE_STROKE_WIDTH:
		error = style_parse_length(value, 1, &style->stroke_width);
		break;
	case STYLE_STROKE_DASHOFFSET:
		error = style_parse_length(value, 0, &style->dash_offset);
		break;
	case STYLE_STROKE_LINECAP:
		error = style_parse_keyword(value, style_caps, sizeof(style_caps) / sizeof(style_caps[0]), &number);
		if (error == 0)
			style->stroke_linecap = (enum vec_cap)number;
		break;
	case STYLE_STROKE_LINEJOIN:
		error = style_parse_keyword(value, style_joins, sizeof(style_joins) / sizeof(style_joins[0]), &number);
		if (error == 0)
			style->stroke_linejoin = (enum vec_join)number;
		break;
	case STYLE_STROKE_MITERLIMIT:
		error = style_parse_miterlimit(value, &style->stroke_miterlimit);
		break;
	case STYLE_STROKE_DASHARRAY:
		error = style_parse_dasharray(value, style);
		break;
	case STYLE_COLOR:
		error = style_parse_color(tree, value, &color, &current);
		if (error == 0) {
			/* currentColor as the value of color is the inherited color. */
			parent_color = STYLE_BLACK;
			if (parent != NULL)
				parent_color = parent->color;
			if (current)
				color = parent_color;
			style->color = color;
		}
		break;
	case STYLE_VISIBILITY:
		error = style_parse_keyword(value, style_visibilities, sizeof(style_visibilities) / sizeof(style_visibilities[0]), &number);
		if (error == 0)
			style->visibility = number;
		break;
	case STYLE_FONT_SIZE:
		parent_size = STYLE_FONT_SIZE_INITIAL;
		if (parent != NULL)
			parent_size = parent->font_size;
		error = style_parse_font_size(value, parent_size, attribute, &style->font_size);
		break;
	case STYLE_DISPLAY:
		error = style_parse_keyword(value, style_displays, sizeof(style_displays) / sizeof(style_displays[0]), &number);
		if (error == 0)
			style->display = number;
		break;
	case STYLE_CLIP_PATH:
		error = style_parse_clip_path(value, style);
		break;
	case STYLE_STOP_COLOR:
		error = style_parse_color(tree, value, &color, &current);
		if (error == 0) {
			/* currentColor stays unresolved: the color property's value at the time of drawing is used. */
			if (current)
				color = STYLE_BLACK;
			style->stop_color = color;
			style->stop_color_current = current;
		}
		break;
	}

	/* Reports an invalid value; the property is as it was. */
	if (error != 0)
		return error;

	/* Succeeded: the property has the value. */
	return 0;
}

/* Reads a value that is one of a property's keywords (ASCII case-insensitive) and gives its code. */
static int
style_parse_keyword(
	const char *value,
	const struct style_keyword *table,
	size_t count,
	int *result)
{
	const char *word;
	size_t length;
	size_t index;
	int found;

	/* Only a value of one word is a keyword. */
	found = style_single_word(value, &word, &length);
	if (!found)
		return EINVAL;

	/* Looks for the word among the keywords. */
	for (index = 0; index < count; index++) {
		found = style_word_equal(word, length, table[index].name);
		if (found) {
			*result = table[index].value;
			return 0;
		}
	}

	/* Refuses a word that is not one of the keywords. */
	return EINVAL;
}

/*
 * Reads an opacity value: a number or a percentage, clamped to [0, 1].
 *
 * Values referenced: CSS Color 4 §3.2 (<opacity-value> = <number> |
 * <percentage>; 100% is 1; a value outside [0, 1] is valid and clamped
 * in the computed value), SVG 2 §3.6.1.
 */
static int
style_parse_opacity(
	const char *value,
	float *opacity)
{
	struct svg_length length;
	float result;
	int error;

	/* Reads a number with an optional percent sign (any other unit is not an opacity). */
	error = svg_parse_length(value, &length);
	if (error != 0)
		return EINVAL;

	/* Turns the number or the percentage into the opacity. */
	if (length.unit == SVG_UNIT_NUMBER) {
		result = length.value;
	} else if (length.unit == SVG_UNIT_PERCENT) {
		result = length.value / 100.0f;
	} else {
		return EINVAL;
	}

	/* Clamps the opacity to the range from transparent to opaque. */
	if (result < 0.0f)
		result = 0.0f;
	if (result > 1.0f)
		result = 1.0f;

	/* Stores the opacity. */
	*opacity = result;

	/* Succeeded: the opacity is set. */
	return 0;
}

/*
 * Reads a length or a number as a length: stroke-width (negative is
 * invalid) and stroke-dashoffset (any sign).
 *
 * Values referenced: SVG 2 §13.5.3 (stroke-width: <length-percentage> |
 * <number>, a negative value is an error), §13.5.6 (stroke-dashoffset:
 * <length-percentage> | <number>).
 */
static int
style_parse_length(
	const char *value,
	int non_negative,
	struct svg_length *length)
{
	struct svg_length parsed;
	int error;

	/* Reads the number and its unit. */
	error = svg_parse_length(value, &parsed);
	if (error != 0)
		return EINVAL;

	/* Refuses a negative length where the property does not take one. */
	if (non_negative && parsed.value < 0.0f)
		return EINVAL;

	/* Stores the length. */
	*length = parsed;

	/* Succeeded: the length is set. */
	return 0;
}

/*
 * Reads stroke-dasharray into the style: none, or a list of lengths none
 * of which is negative.
 *
 * Values referenced: SVG 2 §13.5.6 (none | <dasharray>, <dasharray> = [ [
 * <length-percentage> | <number> ]+ ]#; a negative value makes the
 * whole value invalid).  A list of more than VEC_DASHES_MAX lengths is
 * invalid here, as the stroke style takes no more.
 */
static int
style_parse_dasharray(
	const char *value,
	struct svg_style *style)
{
	struct svg_length lengths[VEC_DASHES_MAX];
	size_t count;
	size_t index;
	int error;

	/* Reads the list into a buffer, so a bad list changes nothing. */
	count = 0;
	error = svg_parse_lengths(value, lengths, VEC_DASHES_MAX, &count);
	if (error != 0)
		return EINVAL;

	/* Refuses a list with a negative length. */
	for (index = 0; index < count; index++) {
		if (lengths[index].value < 0.0f)
			return EINVAL;
	}

	/* Stores the list, with the rest of the room cleared. */
	memset(style->dashes, 0, sizeof(style->dashes));
	for (index = 0; index < count; index++)
		style->dashes[index] = lengths[index];
	style->dash_count = count;

	/* Succeeded: the dash list is set. */
	return 0;
}

/*
 * Reads stroke-miterlimit: a number, not negative.  A value under 1
 * becomes 1: every miter join is longer than the limit then (a miter
 * length over the width, 1 / sin(theta / 2), is more than 1 at every
 * corner), so the corners are all bevels, as for the value that was
 * written, and the stroke style takes limits of at least 1.
 *
 * Values referenced: SVG 2 §13.5.5 (stroke-miterlimit: <number>, a
 * negative value is invalid and ignored; a value between 0 and 1 is no
 * longer an error and makes every miter join exceed the limit).
 */
static int
style_parse_miterlimit(
	const char *value,
	float *limit)
{
	float parsed;
	int error;

	/* Reads the number. */
	error = svg_parse_number(value, &parsed);
	if (error != 0)
		return EINVAL;

	/* Refuses a negative limit. */
	if (parsed < 0.0f)
		return EINVAL;

	/* Raises a limit under 1 to 1, which bevels every corner too. */
	if (parsed < 1.0f)
		parsed = 1.0f;

	/* Stores the limit. */
	*limit = parsed;

	/* Succeeded: the limit is set. */
	return 0;
}

/*
 * Reads a color property's value: a color of the page's parser, or
 * currentColor (told apart by current).
 *
 * Values referenced: SVG 2 §13.3 (currentColor), §14.2.4.2 (stop-color
 * takes a <color>, currentColor included).
 */
static int
style_parse_color(
	const struct svg_tree *tree,
	const char *value,
	uint32_t *color,
	int *current)
{
	const char *word;
	const char *start;
	const char *end;
	size_t length;
	uint32_t parsed;
	int found;
	int space;
	int error;

	/* Tells currentColor, a word, from the colors. */
	found = style_single_word(value, &word, &length);
	if (found) {
		found = style_word_equal(word, length, "currentcolor");
		if (found) {
			*current = 1;
			return 0;
		}
	}

	/* Refuses a value the page's parser cannot be asked about. */
	if (tree == NULL || tree->parse_color == NULL)
		return EINVAL;

	/* Finds the text of the color between the white space around it. */
	start = style_skip_space(value);
	end = start + strlen(start);
	space = 1;
	while (end > start && space) {
		space = style_is_space(end[-1]);
		if (space)
			end--;
	}
	if (end == start)
		return EINVAL;

	/* Asks the page's color parser. */
	error = tree->parse_color(tree->context, start, (size_t)(end - start), &parsed);
	if (error != 0)
		return EINVAL;

	/* Publishes the color. */
	*color = parsed;
	*current = 0;

	/* Succeeded: the value is a color. */
	return 0;
}

/*
 * Reads font-size into user units: a length of absolute units, em, ex,
 * rem or a percentage, not negative.  A bare number is px in a
 * presentation attribute (SVG 2 §4.2) and is valid only as zero in a
 * declaration.  The keyword sizes (medium, larger ...) are not read.
 *
 * Values referenced: CSS Values 3 §5.1.1 (em, ex: the font size of the
 * parent for font-size; ex is half of it, the fallback the section
 * allows), §5.1.2 (rem), §5.2 (the absolute units, as SVG_PX_PER_*),
 * CSS Fonts (font-size: <length-percentage [0, inf]>, a percentage of the
 * parent's size).
 */
static int
style_parse_font_size(
	const char *value,
	float parent_size,
	int attribute,
	float *size)
{
	struct svg_length length;
	double result;
	double amount;
	int error;

	/* Reads the number and its unit. */
	error = svg_parse_length(value, &length);
	if (error != 0)
		return EINVAL;

	/* Refuses a negative size. */
	if (length.value < 0.0f)
		return EINVAL;

	/* Turns the length into px by its unit. */
	amount = (double)length.value;
	result = 0.0;
	switch (length.unit) {
	case SVG_UNIT_NUMBER:
		/* A bare number is a length only in a presentation attribute, or as zero. */
		if (!attribute && length.value != 0.0f)
			return EINVAL;
		result = amount;
		break;
	case SVG_UNIT_PX:
		result = amount;
		break;
	case SVG_UNIT_PERCENT:
		result = (double)parent_size * amount / 100.0;
		break;
	case SVG_UNIT_EM:
		result = (double)parent_size * amount;
		break;
	case SVG_UNIT_EX:
		result = (double)parent_size * amount / 2.0;
		break;
	case SVG_UNIT_REM:
		result = (double)STYLE_FONT_SIZE_ROOT * amount;
		break;
	case SVG_UNIT_IN:
		result = amount * (double)SVG_PX_PER_IN;
		break;
	case SVG_UNIT_CM:
		result = amount * (double)SVG_PX_PER_CM;
		break;
	case SVG_UNIT_MM:
		result = amount * (double)SVG_PX_PER_MM;
		break;
	case SVG_UNIT_Q:
		result = amount * (double)SVG_PX_PER_Q;
		break;
	case SVG_UNIT_PT:
		result = amount * (double)SVG_PX_PER_PT;
		break;
	case SVG_UNIT_PC:
		result = amount * (double)SVG_PX_PER_PC;
		break;
	}

	/* Refuses a size a float cannot hold. */
	if (result > (double)FLT_MAX)
		return EINVAL;

	/* Stores the size. */
	*size = (float)result;

	/* Succeeded: the font size is set. */
	return 0;
}

/*
 * Reads clip-path: none, or a reference to an element of the same
 * document, url(#id).  The basic shapes and the geometry boxes of the
 * property are not supported: such a value is invalid here.
 *
 * Values referenced: CSS Masking 1 §5.1 (<clip-source> | [ <basic-shape>
 * || <geometry-box> ] | none; <clip-source> = <url>).
 */
static int
style_parse_clip_path(
	const char *value,
	struct svg_style *style)
{
	char id[SVG_ID_MAX + 1];
	const char *word;
	const char *cursor;
	size_t length;
	int found;
	int error;

	/* Reads none, which removes the clip. */
	found = style_single_word(value, &word, &length);
	if (found) {
		found = style_word_equal(word, length, "none");
		if (found) {
			style->has_clip_path = 0;
			memset(style->clip_path, 0, sizeof(style->clip_path));
			return 0;
		}
	}

	/* Reads the reference; another document is not supported, any other fault is invalid. */
	cursor = value;
	error = svg_url_read(&cursor, 0, id);
	if (error == ENOTSUP)
		return ENOTSUP;
	if (error != 0)
		return EINVAL;

	/* Refuses anything after the reference. */
	cursor = style_skip_space(cursor);
	if (*cursor != '\0')
		return EINVAL;

	/* Stores the reference. */
	style->has_clip_path = 1;
	memset(style->clip_path, 0, sizeof(style->clip_path));
	memcpy(style->clip_path, id, strlen(id));

	/* Succeeded: the clip refers to the element. */
	return 0;
}

/* Appends a character to a text, or notes that the text does not fit. */
static void
style_text_put(
	struct style_text *text,
	char c)
{
	/* Keeps one byte for the NUL. */
	if (text->length + 1 >= text->size) {
		text->overflow = 1;
		return;
	}

	/* Appends the character. */
	text->buffer[text->length] = c;
	text->length++;
}

/*
 * Copies one declaration, or a whole value, into a buffer with its
 * comments taken out (each becomes a space).  With stop_at_semicolon the
 * copy ends after the first semicolon that is outside parentheses,
 * brackets, braces and quotes, which is how the declarations of a style
 * attribute are told apart (CSS Syntax: a semicolon in a block or a
 * string does not end the declaration); the semicolon is not copied.
 * Otherwise it ends at the NUL.  *cursor moves past what was read, by at
 * least one character when there is any.
 *
 * Errors: ENOBUFS when the text did not fit; the buffer then holds the
 * beginning, and the cursor is past the whole declaration.
 */
static int
style_read_text(
	const char **cursor,
	int stop_at_semicolon,
	char *buffer,
	size_t size)
{
	struct style_text text;
	const char *p;
	char quote;
	char c;
	size_t depth;
	int done;

	/* Starts with an empty buffer. */
	text.buffer = buffer;
	text.size = size;
	text.length = 0;
	text.overflow = 0;
	p = *cursor;
	quote = '\0';
	depth = 0;
	done = 0;

	/* Copies the characters, keeping track of strings and blocks, up to the end of the declaration. */
	while (*p != '\0' && !done) {
		c = *p;
		if (quote != '\0') {
			/* Inside a string: a backslash takes the next character with it; a quote or a line end closes it. */
			if (c == '\\' && p[1] != '\0') {
				style_text_put(&text, c);
				style_text_put(&text, p[1]);
				p += 2;
			} else {
				if (c == quote || c == '\n')
					quote = '\0';
				style_text_put(&text, c);
				p++;
			}
		} else if (c == '/' && p[1] == '*') {
			/* A comment, which reads as a space; one that is not closed runs to the end. */
			p += 2;
			while (*p != '\0' && !(*p == '*' && p[1] == '/'))
				p++;
			if (*p != '\0')
				p += 2;
			style_text_put(&text, ' ');
		} else if (c == '"' || c == '\'') {
			/* A string starts. */
			quote = c;
			style_text_put(&text, c);
			p++;
		} else if (c == '(' || c == '[' || c == '{') {
			/* A block opens. */
			depth++;
			style_text_put(&text, c);
			p++;
		} else if (c == ')' || c == ']' || c == '}') {
			/* A block closes; a closer with no block is an ordinary character. */
			if (depth > 0)
				depth--;
			style_text_put(&text, c);
			p++;
		} else if (c == ';' && stop_at_semicolon && depth == 0) {
			/* The declaration ends. */
			p++;
			done = 1;
		} else {
			style_text_put(&text, c);
			p++;
		}
	}

	/* Ends the text and moves the cursor. */
	text.buffer[text.length] = '\0';
	*cursor = p;

	/* Reports a text that did not fit. */
	if (text.overflow)
		return ENOBUFS;

	/* Succeeded: the buffer holds the text. */
	return 0;
}

/*
 * Cuts a trailing !important (with any white space around the
 * exclamation mark and the keyword) from a value.  The importance has no
 * effect here: a declaration of the style attribute applies either way.
 */
static void
style_strip_important(
	char *value)
{
	char *end;
	char *bang;
	int same;
	int space;

	/* Finds the end of the value without its trailing white space. */
	end = value + strlen(value);
	space = 1;
	while (end > value && space) {
		space = style_is_space(end[-1]);
		if (space)
			end--;
	}

	/* Looks for the word important at the end. */
	if ((size_t)(end - value) < 9)
		return;
	same = style_word_equal(end - 9, 9, "important");
	if (!same)
		return;

	/* Looks for the exclamation mark before it, after any white space. */
	bang = end - 9;
	space = 1;
	while (bang > value && space) {
		space = style_is_space(bang[-1]);
		if (space)
			bang--;
	}
	if (bang == value || bang[-1] != '!')
		return;

	/* Cuts the value there. */
	bang[-1] = '\0';
}

/*
 * Applies one declaration of a style attribute, the text of "name :
 * value" with its comments already taken out.  A declaration with no
 * colon, or with a property this module does not know, is skipped.
 */
static void
style_apply_declaration(
	struct svg_style *style,
	const struct svg_style *parent,
	const struct svg_tree *tree,
	char *text)
{
	const struct style_property_entry *entry;
	const char *name;
	const char *p;
	char *value;
	size_t name_length;
	int space;

	/* Reads the property's name: the characters up to a colon or white space. */
	name = style_skip_space(text);
	p = name;
	space = style_is_space(*p);
	while (*p != '\0' && *p != ':' && !space) {
		p++;
		space = style_is_space(*p);
	}
	name_length = (size_t)(p - name);

	/* Skips a declaration with no colon after the name. */
	p = style_skip_space(p);
	if (*p != ':')
		return;
	p++;

	/* Skips a property this module does not know. */
	entry = style_find_property(name, name_length);
	if (entry == NULL)
		return;

	/* Cuts the importance off the value, which the declaration list owns and may be changed. */
	value = text + (p - text);
	style_strip_important(value);

	/* Applies the value; an invalid one is dropped. */
	(void)style_declare_value(style, parent, tree, entry, value, 0);
}
