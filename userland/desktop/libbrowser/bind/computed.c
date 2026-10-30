/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The computed style of elements for scripts:
 * window.getComputedStyle.
 *
 * The declaration is a CSSStyleDeclaration (the prototype of the inline
 * style, whose getters come here for it) that holds its element and asks
 * the host for the element's style each time a value is read, so it
 * follows the document as other browsers' does.  A value is the CSSOM's
 * resolved value: the used width, height, margins and paddings of an
 * element with a box, and the computed value of anything else, written
 * as other browsers write them (lengths in px, colors as rgb() or
 * rgba()).  An element outside the document reads as empty strings.  The
 * declaration cannot be changed (NoModificationAllowedError), its cssText
 * is empty (as in Chromium), and a pseudo-element's style is the
 * element's.
 */

#include "bind/internal.h"
#include "css/css.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The properties a computed declaration reports, in the order item()
 * lists them.
 */
enum computed_property {
	COMPUTED_DISPLAY,
	COMPUTED_POSITION,
	COMPUTED_FLOAT,
	COMPUTED_CLEAR,
	COMPUTED_VISIBILITY,
	COMPUTED_OVERFLOW,
	COMPUTED_OVERFLOW_X,
	COMPUTED_OVERFLOW_Y,
	COMPUTED_OVERFLOW_INLINE,
	COMPUTED_OVERFLOW_BLOCK,
	COMPUTED_BOX_SIZING,
	COMPUTED_WIDTH,
	COMPUTED_HEIGHT,
	COMPUTED_MIN_WIDTH,
	COMPUTED_MIN_HEIGHT,
	COMPUTED_MAX_WIDTH,
	COMPUTED_MAX_HEIGHT,
	COMPUTED_MARGIN_TOP,
	COMPUTED_MARGIN_RIGHT,
	COMPUTED_MARGIN_BOTTOM,
	COMPUTED_MARGIN_LEFT,
	COMPUTED_PADDING_TOP,
	COMPUTED_PADDING_RIGHT,
	COMPUTED_PADDING_BOTTOM,
	COMPUTED_PADDING_LEFT,
	COMPUTED_TOP,
	COMPUTED_RIGHT,
	COMPUTED_BOTTOM,
	COMPUTED_LEFT,
	COMPUTED_BORDER_TOP_WIDTH,
	COMPUTED_BORDER_RIGHT_WIDTH,
	COMPUTED_BORDER_BOTTOM_WIDTH,
	COMPUTED_BORDER_LEFT_WIDTH,
	COMPUTED_BORDER_TOP_STYLE,
	COMPUTED_BORDER_RIGHT_STYLE,
	COMPUTED_BORDER_BOTTOM_STYLE,
	COMPUTED_BORDER_LEFT_STYLE,
	COMPUTED_BORDER_TOP_COLOR,
	COMPUTED_BORDER_RIGHT_COLOR,
	COMPUTED_BORDER_BOTTOM_COLOR,
	COMPUTED_BORDER_LEFT_COLOR,
	COMPUTED_COLOR,
	COMPUTED_BACKGROUND_COLOR,
	COMPUTED_BACKGROUND_IMAGE,
	COMPUTED_OPACITY,
	COMPUTED_TEXT_SHADOW,
	COMPUTED_Z_INDEX,
	COMPUTED_FONT_SIZE,
	COMPUTED_FONT_WEIGHT,
	COMPUTED_FONT_STYLE,
	COMPUTED_FONT_FAMILY,
	COMPUTED_LINE_HEIGHT,
	COMPUTED_TEXT_ALIGN,
	COMPUTED_TEXT_INDENT,
	COMPUTED_DIRECTION,
	COMPUTED_WHITE_SPACE,
	COMPUTED_CURSOR,
	COMPUTED_VERTICAL_ALIGN,
	COMPUTED_LIST_STYLE_TYPE,
	COMPUTED_BORDER_COLLAPSE,
	COMPUTED_CAPTION_SIDE,
	COMPUTED_FLEX_DIRECTION,
	COMPUTED_FLEX_WRAP,
	COMPUTED_FLEX_GROW,
	COMPUTED_FLEX_SHRINK,
	COMPUTED_FLEX_BASIS,
	COMPUTED_JUSTIFY_CONTENT,
	COMPUTED_JUSTIFY_ITEMS,
	COMPUTED_ALIGN_ITEMS,
	COMPUTED_ALIGN_SELF,
	COMPUTED_JUSTIFY_SELF,
	COMPUTED_ALIGN_CONTENT,
	COMPUTED_ORDER,
	COMPUTED_ROW_GAP,
	COMPUTED_COLUMN_GAP,
	COMPUTED_OUTLINE_WIDTH,
	COMPUTED_OUTLINE_STYLE,
	COMPUTED_OUTLINE_COLOR,
	COMPUTED_TRANSFORM,
	COMPUTED_GRID_TEMPLATE_COLUMNS,
	COMPUTED_TEXT_TRANSFORM,
	COMPUTED_TEXT_DECORATION,
	COMPUTED_TEXT_DECORATION_LINE,
	COMPUTED_TEXT_DECORATION_STYLE,
	COMPUTED_TEXT_DECORATION_SKIP_INK,
	COMPUTED_TEXT_DECORATION_COLOR,
	COMPUTED_WRITING_MODE,
	COMPUTED_TEXT_ORIENTATION,
	COMPUTED_GRID_TEMPLATE_ROWS,
	COMPUTED_GRID_AUTO_FLOW,
	COMPUTED_GRID_AUTO_COLUMNS,
	COMPUTED_GRID_AUTO_ROWS,
	COMPUTED_GRID_TEMPLATE_AREAS,
	COMPUTED_GRID_ROW_START,
	COMPUTED_GRID_ROW_END,
	COMPUTED_GRID_COLUMN_START,
	COMPUTED_GRID_COLUMN_END,
	COMPUTED_GRID_ROW,
	COMPUTED_GRID_COLUMN,
	COMPUTED_GRID_AREA,
	COMPUTED_FLEX,
	COMPUTED_FLEX_FLOW,
	COMPUTED_ANCHOR_NAME,
	COMPUTED_POSITION_ANCHOR,
	COMPUTED_ANCHOR_SCOPE,
	COMPUTED_POSITION_AREA,
	COMPUTED_POSITION_TRY_FALLBACKS,
	COMPUTED_POSITION_TRY_ORDER,
	COMPUTED_POSITION_TRY,
	COMPUTED_POSITION_VISIBILITY,
	COMPUTED_VIEW_TRANSITION_NAME,
	COMPUTED_VIEW_TRANSITION_CLASS,
	COMPUTED_ANIMATION_NAME,
	COMPUTED_ANIMATION_DURATION,
	COMPUTED_ANIMATION_DELAY,
	COMPUTED_ANIMATION_PLAY_STATE,
	COMPUTED_ANIMATION_TIMING_FUNCTION,
	COMPUTED_MIX_BLEND_MODE,
	COMPUTED_ZOOM,

	/* The flow-relative properties, six families of four (inline-start, inline-end, block-start, block-end): margin, padding, inset, border width, style and color. */
	COMPUTED_LOGICAL_FIRST,
	COMPUTED_LOGICAL_LAST = COMPUTED_LOGICAL_FIRST + 23
};

/*
 * One property of a computed declaration: its CSS name and what it is.
 */
struct computed_entry {
	const char *name;
	int property;
};

/*
 * What a computed declaration holds: its element's object.
 */
struct computed_ref {
	struct vm_cell cell;
	vm_value element;
	/* A view transition pseudo-element asked for (CSS_PSEUDO_ELEMENT_VT_*; -1 for one that did not parse), and its name. */
	int pseudo;
	struct vm_string *pseudo_name;
};

/* The pseudo-element of the declaration being read (set by bind_computed_element, read by computed_write). */
static int computed_pseudo;
static struct vm_string *computed_pseudo_name;

static void computed_trace(struct vm_heap *heap, struct vm_cell *cell);
static int computed_find(const struct vm_string *name, int *property);
static int computed_write_grid(struct bind_window *window, struct dom_element *element, const struct css_style *style, int property, struct wb_units *out, int *handled);
static int computed_write(struct bind_window *window, struct dom_element *element, int property, struct wb_units *out);
static int computed_write_used(const struct css_style *style, const struct bind_box *box, int found, int property, struct wb_units *out);
static int computed_write_keyword(const struct css_style *style, int property, struct wb_units *out);
static int computed_overflow_axis(int own, int other);
static int computed_logical_property(const struct css_style *style, int property);
static int computed_write_overflow(const struct css_style *style, struct wb_units *out);
static int computed_write_url(const struct vm_string *url, struct wb_units *out);
static int computed_needs_quotes(const struct vm_string *family);
static int computed_write_percent(const struct css_length *length, struct wb_units *out);
static int computed_write_url_quoted(const struct vm_string *text, struct wb_units *out);
static int computed_write_offset(const struct css_style *style, int side, struct wb_units *out);
static int computed_write_font_family(const struct css_style *style, struct wb_units *out);
static int computed_write_tracks(const struct css_track *tracks, int count, const struct css_line_names *names, struct wb_units *out);
static int computed_anchor_names(const struct css_anchor_names *names, const char *none, struct wb_units *out);
static void computed_line_names(const struct css_line_names *names, const struct css_track_repeat *repeat, int repeats, int line, struct css_line_names *out);
static void computed_names_add(struct css_line_names *out, const struct css_line_names *from, int first, int end);

static int computed_track_minimum(const struct css_track *track, struct wb_units *out);
static int computed_track_breadth(const struct css_track *track, struct wb_units *out);
static int computed_name(const char *const *names, size_t count, int value, struct wb_units *out);
static int computed_align(int value, int flex, struct wb_units *out);
static void computed_blockify(struct bind_window *window, struct dom_element *element, struct css_style *style);
static int computed_length(const struct css_length *length, struct wb_units *out);
static int computed_pixels(double value, struct wb_units *out);
static int computed_number(double value, struct wb_units *out);
static int computed_color(uint32_t color, struct wb_units *out);
static int computed_ascii(const char *ascii, struct wb_units *out);
static int computed_decoration_append(struct wb_units *out, const struct wb_units *part);
static int computed_decoration(const struct css_style *style, struct wb_units *out);
static int computed_text_shadow(const struct css_style *style, struct wb_units *out);

/* The state of a computed declaration, which marks its element's object. */
static const struct vm_cell_type computed_ref_type = { "computed-style", computed_trace, NULL };

/*
 * The properties, in the order item() lists them.  The table is constant
 * for the life of the program.
 */
static const struct computed_entry computed_entries[] = {
	{ "margin-inline-start", COMPUTED_LOGICAL_FIRST + 0 },
	{ "margin-inline-end", COMPUTED_LOGICAL_FIRST + 1 },
	{ "margin-block-start", COMPUTED_LOGICAL_FIRST + 2 },
	{ "margin-block-end", COMPUTED_LOGICAL_FIRST + 3 },
	{ "padding-inline-start", COMPUTED_LOGICAL_FIRST + 4 },
	{ "padding-inline-end", COMPUTED_LOGICAL_FIRST + 5 },
	{ "padding-block-start", COMPUTED_LOGICAL_FIRST + 6 },
	{ "padding-block-end", COMPUTED_LOGICAL_FIRST + 7 },
	{ "inset-inline-start", COMPUTED_LOGICAL_FIRST + 8 },
	{ "inset-inline-end", COMPUTED_LOGICAL_FIRST + 9 },
	{ "inset-block-start", COMPUTED_LOGICAL_FIRST + 10 },
	{ "inset-block-end", COMPUTED_LOGICAL_FIRST + 11 },
	{ "border-inline-start-width", COMPUTED_LOGICAL_FIRST + 12 },
	{ "border-inline-end-width", COMPUTED_LOGICAL_FIRST + 13 },
	{ "border-block-start-width", COMPUTED_LOGICAL_FIRST + 14 },
	{ "border-block-end-width", COMPUTED_LOGICAL_FIRST + 15 },
	{ "border-inline-start-style", COMPUTED_LOGICAL_FIRST + 16 },
	{ "border-inline-end-style", COMPUTED_LOGICAL_FIRST + 17 },
	{ "border-block-start-style", COMPUTED_LOGICAL_FIRST + 18 },
	{ "border-block-end-style", COMPUTED_LOGICAL_FIRST + 19 },
	{ "border-inline-start-color", COMPUTED_LOGICAL_FIRST + 20 },
	{ "border-inline-end-color", COMPUTED_LOGICAL_FIRST + 21 },
	{ "border-block-start-color", COMPUTED_LOGICAL_FIRST + 22 },
	{ "border-block-end-color", COMPUTED_LOGICAL_FIRST + 23 },
	{ "display", COMPUTED_DISPLAY },
	{ "position", COMPUTED_POSITION },
	{ "float", COMPUTED_FLOAT },
	{ "clear", COMPUTED_CLEAR },
	{ "visibility", COMPUTED_VISIBILITY },
	{ "overflow", COMPUTED_OVERFLOW },
	{ "overflow-x", COMPUTED_OVERFLOW_X },
	{ "overflow-y", COMPUTED_OVERFLOW_Y },
	{ "overflow-inline", COMPUTED_OVERFLOW_INLINE },
	{ "overflow-block", COMPUTED_OVERFLOW_BLOCK },
	{ "box-sizing", COMPUTED_BOX_SIZING },
	{ "width", COMPUTED_WIDTH },
	{ "height", COMPUTED_HEIGHT },
	{ "min-width", COMPUTED_MIN_WIDTH },
	{ "min-height", COMPUTED_MIN_HEIGHT },
	{ "max-width", COMPUTED_MAX_WIDTH },
	{ "max-height", COMPUTED_MAX_HEIGHT },
	{ "margin-top", COMPUTED_MARGIN_TOP },
	{ "margin-right", COMPUTED_MARGIN_RIGHT },
	{ "margin-bottom", COMPUTED_MARGIN_BOTTOM },
	{ "margin-left", COMPUTED_MARGIN_LEFT },
	{ "padding-top", COMPUTED_PADDING_TOP },
	{ "padding-right", COMPUTED_PADDING_RIGHT },
	{ "padding-bottom", COMPUTED_PADDING_BOTTOM },
	{ "padding-left", COMPUTED_PADDING_LEFT },
	{ "top", COMPUTED_TOP },
	{ "right", COMPUTED_RIGHT },
	{ "bottom", COMPUTED_BOTTOM },
	{ "left", COMPUTED_LEFT },
	{ "border-top-width", COMPUTED_BORDER_TOP_WIDTH },
	{ "border-right-width", COMPUTED_BORDER_RIGHT_WIDTH },
	{ "border-bottom-width", COMPUTED_BORDER_BOTTOM_WIDTH },
	{ "border-left-width", COMPUTED_BORDER_LEFT_WIDTH },
	{ "border-top-style", COMPUTED_BORDER_TOP_STYLE },
	{ "border-right-style", COMPUTED_BORDER_RIGHT_STYLE },
	{ "border-bottom-style", COMPUTED_BORDER_BOTTOM_STYLE },
	{ "border-left-style", COMPUTED_BORDER_LEFT_STYLE },
	{ "border-top-color", COMPUTED_BORDER_TOP_COLOR },
	{ "border-right-color", COMPUTED_BORDER_RIGHT_COLOR },
	{ "border-bottom-color", COMPUTED_BORDER_BOTTOM_COLOR },
	{ "border-left-color", COMPUTED_BORDER_LEFT_COLOR },
	{ "color", COMPUTED_COLOR },
	{ "background-color", COMPUTED_BACKGROUND_COLOR },
	{ "background-image", COMPUTED_BACKGROUND_IMAGE },
	{ "opacity", COMPUTED_OPACITY },
	{ "text-shadow", COMPUTED_TEXT_SHADOW },
	{ "z-index", COMPUTED_Z_INDEX },
	{ "font-size", COMPUTED_FONT_SIZE },
	{ "font-weight", COMPUTED_FONT_WEIGHT },
	{ "font-style", COMPUTED_FONT_STYLE },
	{ "font-family", COMPUTED_FONT_FAMILY },
	{ "line-height", COMPUTED_LINE_HEIGHT },
	{ "text-align", COMPUTED_TEXT_ALIGN },
	{ "text-indent", COMPUTED_TEXT_INDENT },
	{ "direction", COMPUTED_DIRECTION },
	{ "writing-mode", COMPUTED_WRITING_MODE },
	{ "text-orientation", COMPUTED_TEXT_ORIENTATION },
	{ "white-space", COMPUTED_WHITE_SPACE },
	{ "text-transform", COMPUTED_TEXT_TRANSFORM },
	{ "cursor", COMPUTED_CURSOR },
	{ "vertical-align", COMPUTED_VERTICAL_ALIGN },
	{ "list-style-type", COMPUTED_LIST_STYLE_TYPE },
	{ "border-collapse", COMPUTED_BORDER_COLLAPSE },
	{ "caption-side", COMPUTED_CAPTION_SIDE },
	{ "flex-direction", COMPUTED_FLEX_DIRECTION },
	{ "flex-wrap", COMPUTED_FLEX_WRAP },
	{ "flex-grow", COMPUTED_FLEX_GROW },
	{ "flex-shrink", COMPUTED_FLEX_SHRINK },
	{ "flex-basis", COMPUTED_FLEX_BASIS },
	{ "justify-content", COMPUTED_JUSTIFY_CONTENT },
	{ "justify-items", COMPUTED_JUSTIFY_ITEMS },
	{ "align-items", COMPUTED_ALIGN_ITEMS },
	{ "align-self", COMPUTED_ALIGN_SELF },
	{ "justify-self", COMPUTED_JUSTIFY_SELF },
	{ "align-content", COMPUTED_ALIGN_CONTENT },
	{ "order", COMPUTED_ORDER },
	{ "row-gap", COMPUTED_ROW_GAP },
	{ "column-gap", COMPUTED_COLUMN_GAP },
	{ "outline-width", COMPUTED_OUTLINE_WIDTH },
	{ "outline-style", COMPUTED_OUTLINE_STYLE },
	{ "outline-color", COMPUTED_OUTLINE_COLOR },
	{ "transform", COMPUTED_TRANSFORM },
	{ "grid-template-columns", COMPUTED_GRID_TEMPLATE_COLUMNS },
	{ "grid-template-rows", COMPUTED_GRID_TEMPLATE_ROWS },
	{ "grid-auto-flow", COMPUTED_GRID_AUTO_FLOW },
	{ "grid-auto-columns", COMPUTED_GRID_AUTO_COLUMNS },
	{ "grid-auto-rows", COMPUTED_GRID_AUTO_ROWS },
	{ "grid-template-areas", COMPUTED_GRID_TEMPLATE_AREAS },
	{ "grid-row-start", COMPUTED_GRID_ROW_START },
	{ "grid-row-end", COMPUTED_GRID_ROW_END },
	{ "grid-column-start", COMPUTED_GRID_COLUMN_START },
	{ "grid-column-end", COMPUTED_GRID_COLUMN_END },
	{ "grid-row", COMPUTED_GRID_ROW },
	{ "grid-column", COMPUTED_GRID_COLUMN },
	{ "grid-area", COMPUTED_GRID_AREA },
	{ "flex", COMPUTED_FLEX },
	{ "flex-flow", COMPUTED_FLEX_FLOW },
	{ "anchor-name", COMPUTED_ANCHOR_NAME },
	{ "position-anchor", COMPUTED_POSITION_ANCHOR },
	{ "anchor-scope", COMPUTED_ANCHOR_SCOPE },
	{ "position-area", COMPUTED_POSITION_AREA },
	{ "position-try-fallbacks", COMPUTED_POSITION_TRY_FALLBACKS },
	{ "position-try-order", COMPUTED_POSITION_TRY_ORDER },
	{ "position-try", COMPUTED_POSITION_TRY },
	{ "position-visibility", COMPUTED_POSITION_VISIBILITY },
	{ "view-transition-name", COMPUTED_VIEW_TRANSITION_NAME },
	{ "view-transition-class", COMPUTED_VIEW_TRANSITION_CLASS },
	{ "animation-name", COMPUTED_ANIMATION_NAME },
	{ "animation-duration", COMPUTED_ANIMATION_DURATION },
	{ "animation-delay", COMPUTED_ANIMATION_DELAY },
	{ "animation-play-state", COMPUTED_ANIMATION_PLAY_STATE },
	{ "animation-timing-function", COMPUTED_ANIMATION_TIMING_FUNCTION },
	{ "mix-blend-mode", COMPUTED_MIX_BLEND_MODE },
	{ "zoom", COMPUTED_ZOOM },
	{ "text-decoration-line", COMPUTED_TEXT_DECORATION_LINE },
	{ "text-decoration-style", COMPUTED_TEXT_DECORATION_STYLE },
	{ "text-decoration-skip-ink", COMPUTED_TEXT_DECORATION_SKIP_INK },
	{ "text-decoration-color", COMPUTED_TEXT_DECORATION_COLOR },
	{ NULL, 0 }
};

/* The names of display, by enum css_display. */
static const char *const computed_displays[] = {
	"inline", "block", "inline-block", "list-item", "none", "table", "table-row", "table-cell", "flex", "contents", "grid",
	"table-row-group", "table-caption", "inline-table", "table-column", "inline-grid", "inline-flex"
};

/* The names of position, by enum css_position. */
static const char *const computed_positions[] = { "static", "relative", "absolute", "fixed", "sticky" };

/* The names of float and clear, by enum css_float. */
static const char *const computed_floats[] = { "none", "left", "right", "both" };

/* The names of visibility. */
static const char *const computed_visibilities[] = { "visible", "hidden" };

/* The names of overflow, by enum css_overflow. */
static const char *const computed_overflows[] = { "visible", "hidden", "clip", "scroll", "auto" };

/* The names of box-sizing, by enum css_box_sizing. */
static const char *const computed_box_sizings[] = { "content-box", "border-box" };

/* The names of the border styles, by enum css_border_style. */
static const char *const computed_border_styles[] = { "none", "solid", "dashed", "dotted", "double", "inset", "outset" };

/* The names of text-align, by enum css_text_align. */
static const char *const computed_text_aligns[] = { "start", "left", "right", "center", "justify", "end" };

/* The names of direction, by enum css_direction. */
static const char *const computed_directions[] = { "ltr", "rtl" };

/* The writing mode names, by enum css_writing_mode. */
static const char *const computed_writing_modes[] = { "horizontal-tb", "vertical-lr", "vertical-rl" };

/* The text orientation names, by enum css_text_orientation. */
static const char *const computed_text_orientations[] = { "mixed", "upright", "sideways" };

/* The canonical cursor names, indexed by enum css_cursor for computed serialization. */
static const char *const computed_cursors[] = {
	"auto",
	"default",
	"none",
	"context-menu",
	"help",
	"pointer",
	"progress",
	"wait",
	"cell",
	"crosshair",
	"text",
	"vertical-text",
	"alias",
	"copy",
	"move",
	"no-drop",
	"not-allowed",
	"e-resize",
	"n-resize",
	"ne-resize",
	"nw-resize",
	"s-resize",
	"se-resize",
	"sw-resize",
	"w-resize",
	"ew-resize",
	"ns-resize",
	"nesw-resize",
	"nwse-resize",
	"col-resize",
	"row-resize",
	"all-scroll",
	"grab",
	"grabbing",
	"zoom-in",
	"zoom-out",
};

/* The resolved CSS2 transform names follow the native computed keyword enum. */
static const char *const computed_text_transforms[] = { "none", "capitalize", "uppercase", "lowercase" };

/* The text decoration keyword names follow their computed enums. */
static const char *const computed_decoration_styles[] = { "solid", "double", "dotted", "dashed", "wavy" };
static const char *const computed_decoration_skip_inks[] = { "auto", "none", "all" };

/* The names of white-space, by enum css_white_space. */
static const char *const computed_white_spaces[] = { "normal", "pre", "nowrap", "pre-wrap", "pre-line" };

/* The names of vertical-align's keywords, by enum css_vertical_align. */
static const char *const computed_vertical_aligns[] = {
	"baseline", "top", "middle", "bottom", "text-top", "text-bottom", "sub", "super"
};

/* The names of list-style-type, by enum css_list_style. */
static const char *const computed_list_styles[] = { "disc", "circle", "square", "decimal", "none" };

/* The names of border-collapse. */
static const char *const computed_collapses[] = { "separate", "collapse" };
static const char *const computed_caption_sides[] = { "top", "bottom" };

/* The names of flex-direction, by enum css_flex_direction. */
static const char *const computed_flex_directions[] = { "row", "row-reverse", "column", "column-reverse" };

/* The names of flex-wrap, by enum css_flex_wrap. */
static const char *const computed_flex_wraps[] = { "nowrap", "wrap", "wrap-reverse" };

/* The names of the alignments, by enum css_align (stretch is normal's). */
static const char *const computed_aligns[] = {
	"auto", "normal", "stretch", "flex-start", "flex-end", "center", "baseline", "space-between", "space-around", "space-evenly"
};

/* Grid self-alignment uses start and end rather than Flexbox's flex-start and flex-end. */
static const char *const computed_justifies[] = {
	"auto", "normal", "stretch", "start", "end", "center", "baseline", "space-between", "space-around", "space-evenly", "left", "right", "self-start", "self-end"
};

/*
 * Reads getComputedStyle's pseudo-element argument: 0 when it is none or ::before or ::after (they are not told
 * apart here), a view transition pseudo-element (CSS_PSEUDO_ELEMENT_VT_*) with its name (NULL for *), or -1 when
 * it does not parse as one.
 */
static int
computed_pseudo_of(
	struct vm_realm *realm,
	vm_value argument,
	struct vm_string **name)
{
	struct vm_string *string;
	uint16_t units[128];
	char word[64];
	size_t length;
	size_t index;
	size_t start;
	size_t used;
	int pseudo;
	int status;

	*name = NULL;
	if (argument == VM_VALUE_UNDEFINED || argument == VM_VALUE_NULL)
		return 0;
	status = vm_to_string(realm, argument, &string);
	if (status != 0)
		return 0;
	length = string->length;
	if (length == 0)
		return 0;
	if (length > 127U)
		return -1;
	for (index = 0; index < length; index++)
		units[index] = vm_string_at(string, index);
	index = 0;
	while (index < length && units[index] == ' ')
		index++;
	if (index + 1U >= length || units[index] != ':' || units[index + 1U] != ':')
		return units[index] == ':' ? 0 : -1;
	index += 2U;
	used = 0;
	while (index < length && units[index] != '(' && units[index] != ' ' && used < sizeof(word) - 1U)
		word[used++] = (char)units[index++];
	word[used] = '\0';
	if (strncmp(word, "view-transition", 15) != 0)
		return 0;
	pseudo = 0;
	if (strcmp(word, "view-transition") == 0)
		pseudo = CSS_PSEUDO_ELEMENT_VT_ROOT;
	else if (strcmp(word, "view-transition-group") == 0)
		pseudo = CSS_PSEUDO_ELEMENT_VT_GROUP;
	else if (strcmp(word, "view-transition-image-pair") == 0)
		pseudo = CSS_PSEUDO_ELEMENT_VT_PAIR;
	else if (strcmp(word, "view-transition-old") == 0)
		pseudo = CSS_PSEUDO_ELEMENT_VT_OLD;
	else if (strcmp(word, "view-transition-new") == 0)
		pseudo = CSS_PSEUDO_ELEMENT_VT_NEW;
	else
		return -1;
	while (index < length && units[index] == ' ')
		index++;
	if (pseudo == CSS_PSEUDO_ELEMENT_VT_ROOT)
		return index == length ? pseudo : -1;
	if (index >= length || units[index] != '(')
		return -1;
	index++;
	while (index < length && units[index] == ' ')
		index++;
	start = index;
	while (index < length && units[index] != ')' && units[index] != ' ')
		index++;
	if (index == start)
		return -1;
	if (!(index - start == 1U && units[start] == '*')) {
		*name = vm_atom_from_units(realm->heap, units + start, index - start);
		if (*name == NULL)
			return -1;
	}
	while (index < length && units[index] == ' ')
		index++;
	if (index >= length || units[index] != ')')
		return -1;
	index++;
	while (index < length && units[index] == ' ')
		index++;
	return index == length ? pseudo : -1;
}

/*
 * Makes the computed declaration of an element's style
 * (window.getComputedStyle); the pseudo-element argument is not read.
 */
int
bind_get_computed_style(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct computed_ref *ref;
	struct vm_object *declaration;
	struct vm_cell *ref_root;
	struct dom_node *node;
	vm_value given;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The element. */
	window = bind_window_of(realm);
	given = js_argument(args, count, 0);
	node = bind_node_of(given);
	if (node == NULL || node->type != DOM_ELEMENT) {
		status = vm_throw_type_error(realm, "Failed to execute 'getComputedStyle' on 'Window': parameter 1 is not of type 'Element'.");
		return status;
	}

	/* The state, which keeps the element's object. */
	ref = vm_heap_alloc(realm->heap, &computed_ref_type, sizeof(*ref));
	if (ref == NULL)
		return ENOMEM;
	ref->element = given;
	ref->pseudo = computed_pseudo_of(realm, js_argument(args, count, 1), &ref->pseudo_name);
	ref_root = &ref->cell;
	status = vm_heap_add_root(realm->heap, &ref_root);
	if (status != 0)
		return status;

	/* The declaration with CSSStyleDeclaration's prototype. */
	declaration = vm_object_create(realm->heap, window->prototypes[BIND_CSS_STYLE_DECLARATION]);
	if (declaration == NULL) {
		vm_heap_remove_root(realm->heap, &ref_root);
		return ENOMEM;
	}

	/* The published declaration now traces its private computed-style state. */
	declaration->kind = VM_KIND_PLATFORM;
	declaration->internal = vm_value_cell(ref);

	/* Succeeded: the declaration. */
	*result = vm_value_cell(declaration);
	vm_heap_remove_root(realm->heap, &ref_root);
	return 0;
}

/*
 * Tells whether a value is a computed declaration, and finds its element
 * when it is.
 */
int
bind_computed_element(
	vm_value value,
	struct dom_element **element)
{
	struct computed_ref *ref;
	struct vm_object *object;
	struct vm_cell *cell;
	int is_object;
	int is_cell;

	/* A platform object. */
	*element = NULL;
	is_object = vm_value_is_object(value);
	if (!is_object)
		return 0;
	object = (struct vm_object *)vm_value_as_cell(value);
	if (object->kind != VM_KIND_PLATFORM)
		return 0;

	/* Whose cell is a computed declaration's state. */
	is_cell = vm_value_is_cell(object->internal);
	if (!is_cell)
		return 0;
	cell = vm_value_as_cell(object->internal);
	if (cell->type != &computed_ref_type)
		return 0;

	/* Succeeded: its element. */
	ref = (struct computed_ref *)cell;
	*element = (struct dom_element *)bind_node_of(ref->element);
	computed_pseudo = ref->pseudo;
	computed_pseudo_name = ref->pseudo_name;
	return 1;
}

/*
 * Reports the resolved value of a property (by its CSS name) of an
 * element's style, or the empty string for a property this pass does not
 * report or an element outside the document.
 */
int
bind_computed_value(
	struct vm_realm *realm,
	struct dom_element *element,
	const struct vm_string *name,
	vm_value *result)
{
	struct bind_window *window;
	struct wb_units units;
	int property;
	int found;
	int status;

	/* A property not reported is empty. */
	found = computed_find(name, &property);
	if (!found) {
		status = bind_string(realm, "", result);
		return status;
	}

	/* Uses the element's owning Document even for a borrowed parent method. */
	window = bind_window_of(realm);
	if (element->node.document->view != NULL)
		window = element->node.document->view;

	/* The value's text. */
	wb_units_init(&units);
	status = computed_write(window, element, property, &units);
	if (status != 0) {
		wb_units_release(&units);
		return status;
	}

	/* Succeeded: as a string. */
	status = bind_units(realm, units.data, units.length, result);
	wb_units_release(&units);
	if (status != 0)
		return status;
	return 0;
}

/* Reports how many properties a computed declaration lists (length). */
size_t
bind_computed_count(void)
{
	/* The table's entries. */
	return sizeof(computed_entries) / sizeof(computed_entries[0]) - 1U;
}

/* Reports the name of the property at a place of the list, or NULL past its end (item). */
const char *
bind_computed_name(
	size_t index)
{
	size_t count;

	/* Past the end there is none. */
	count = bind_computed_count();
	if (index >= count)
		return NULL;

	/* Succeeded: the name. */
	return computed_entries[index].name;
}

/* Marks the element's object a computed declaration holds. */
static void
computed_trace(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct computed_ref *ref;

	/* The element. */
	ref = (struct computed_ref *)cell;
	vm_heap_mark_value(heap, ref->element);
}

/* Finds the property a CSS name reports; reports whether there is one. */
static int
computed_find(
	const struct vm_string *name,
	int *property)
{
	size_t index;
	int same;

	/* Shorthands can be read but do not appear in the longhand item() list. */
	if (vm_string_equal_ascii(name, "text-decoration")) {
		*property = COMPUTED_TEXT_DECORATION;
		return 1;
	}

	/* Each entry's name. */
	for (index = 0; computed_entries[index].name != NULL; index++) {
		same = vm_string_equal_ascii(name, computed_entries[index].name);
		if (same) {
			*property = computed_entries[index].property;
			return 1;
		}
	}

	/* No such property. */
	return 0;
}

/* Turns an inline-level display into its block-level equivalent (CSS Display 3). */
static void
computed_blockify(
	struct bind_window *window,
	struct dom_element *element,
	struct css_style *style)
{
	struct css_style *parent_style;
	struct dom_node *parent;
	int blockify;
	int error;

	if (style->display != CSS_DISPLAY_INLINE && style->display != CSS_DISPLAY_INLINE_BLOCK &&
	    style->display != CSS_DISPLAY_INLINE_TABLE && style->display != CSS_DISPLAY_INLINE_FLEX &&
	    style->display != CSS_DISPLAY_INLINE_GRID)
		return;
	blockify = style->float_side != CSS_FLOAT_NONE || style->position == CSS_POSITION_ABSOLUTE ||
	    style->position == CSS_POSITION_FIXED;
	parent = element->node.parent;
	if (!blockify && parent != NULL && parent->type == DOM_ELEMENT) {
		parent_style = malloc(sizeof(*parent_style));
		if (parent_style == NULL)
			return;
		if (window->host.computed_style != NULL)
			error = window->host.computed_style(window->host.context, (struct dom_element *)parent, parent_style);
		else
			error = bind_style_context_compute(window, (struct dom_element *)parent, parent_style);
		if (error == 0 && (parent_style->display == CSS_DISPLAY_FLEX ||
		    parent_style->display == CSS_DISPLAY_INLINE_FLEX ||
		    parent_style->display == CSS_DISPLAY_GRID || parent_style->display == CSS_DISPLAY_INLINE_GRID))
			blockify = 1;
		free(parent_style);
	}
	if (!blockify)
		return;
	if (style->display == CSS_DISPLAY_INLINE_TABLE)
		style->display = CSS_DISPLAY_TABLE;
	else if (style->display == CSS_DISPLAY_INLINE_FLEX)
		style->display = CSS_DISPLAY_FLEX;
	else if (style->display == CSS_DISPLAY_INLINE_GRID)
		style->display = CSS_DISPLAY_GRID;
	else
		style->display = CSS_DISPLAY_BLOCK;
}

/*
 * Writes the resolved value of a property of an element: nothing for an
 * element outside the document.
 */
static int
computed_write(
	struct bind_window *window,
	struct dom_element *element,
	int property,
	struct wb_units *out)
{
	struct css_style *style;
	struct bind_box box;
	int found;
	int handled;
	int error;

	/* Only active initial children can compute without a primary page host. */
	if (window->host.computed_style == NULL) {
		if (!window->owned ||
		    window->context_depth == 0 ||
		    window->detached)
			return 0;
	}

	/* The element's style (on the heap: it is large). */
	style = malloc(sizeof(*style));
	if (style == NULL)
		return ENOMEM;

	/* Primary pages keep their host path; initial children own their cascade. */
	if (computed_pseudo < 0) {
		/* A pseudo-element argument that did not parse has no values. */
		free(style);
		return 0;
	}
	if (computed_pseudo != 0 && window->host.vt_pseudo_style != NULL) {
		error = window->host.vt_pseudo_style(window->host.context, element, computed_pseudo, computed_pseudo_name, style);
	} else if (window->host.computed_style != NULL) {
		error = window->host.computed_style(window->host.context, element, style);
	} else {
		error = bind_style_context_compute(window, element, style);
	}

	/* Detached or disconnected elements have empty resolved values. */
	if (error == ENOENT) {
		free(style);
		return 0;
	}

	/* Any other failure. */
	if (error != 0) {
		free(style);
		return error;
	}

	/* A flow-relative property is the physical one it maps to here. */
	if (property >= COMPUTED_LOGICAL_FIRST && property <= COMPUTED_LOGICAL_LAST)
		property = computed_logical_property(style, property);

	/* The computed display of a floated, positioned or flex or grid item box is blockified. */
	if (property == COMPUTED_DISPLAY)
		computed_blockify(window, element, style);

	/* Grid template axes resolve from actual track sizing when the host can copy them. */
	error = computed_write_grid(window, element, style, property, out, &handled);
	if (error != 0) {
		free(style);
		return error;
	}

	/* A resolved Grid query already wrote the complete property value. */
	if (handled) {
		free(style);
		return 0;
	}

	/* Its box, for the used values. */
	memset(&box, 0, sizeof(box));
	found = 0;
	if (window->host.node_box != NULL)
		found = window->host.node_box(window->host.context, &element->node, &box);

	/* Serialize the ordinary resolved value before releasing its style copy. */
	error = computed_write_used(style, &box, found, property, out);
	free(style);
	/* A failed property serialization cannot publish a complete value. */
	if (error != 0)
		return error;

	/* Succeeded: the ordinary resolved property value is complete. */
	return 0;
}

/* Writes a supported Grid template axis from copied actual layout values. */
static int
computed_write_grid(
	struct bind_window *window,
	struct dom_element *element,
	const struct css_style *style,
	int property,
	struct wb_units *out,
	int *handled)
{
	struct wb_vector sizes;
	const struct css_line_names *names;
	const double *tracks;
	size_t index;
	int explicit_count;
	int columns;
	const struct css_track_repeat *repeat;
	struct css_line_names named;
	int leading;
	int repeats;
	int line;
	int found;
	int error;

	/* Other properties, including grid-auto axes, retain their computed serialization. */
	*handled = 0;
	if (property != COMPUTED_GRID_TEMPLATE_COLUMNS && property != COMPUTED_GRID_TEMPLATE_ROWS)
		return 0;

	/* A subgrid axis is its line-name lists. */
	if (property == COMPUTED_GRID_TEMPLATE_COLUMNS ? style->column_subgrid != NULL : style->row_subgrid != NULL) {
		*handled = 1;
		return computed_ascii(property == COMPUTED_GRID_TEMPLATE_COLUMNS ? style->column_subgrid : style->row_subgrid, out);
	}

	/* Hosts without this private query retain the established computed-value behavior. */
	if (window->host.grid_tracks == NULL)
		return 0;

	/* The host appends doubles into caller-owned storage instead of returning arena pointers. */
	wb_vector_init(&sizes, sizeof(double));
	columns = 0;
	if (property == COMPUTED_GRID_TEMPLATE_COLUMNS)
		columns = 1;

	/* Copy one axis from the current final layout. */
	found = 0;
	error = window->host.grid_tracks(window->host.context, &element->node, columns, &sizes, &leading, &repeats, &found);
	if (error != 0)
		goto cleanup;

	/* Unsupported axes and missing boxes request the ordinary computed fallback. */
	if (!found)
		goto cleanup;

	/* Empty Grid axes have no actual tracks and resolve to none. */
	*handled = 1;
	if (sizes.count == 0) {
		error = computed_ascii("none", out);
		goto cleanup;
	}

	/* The explicit lines' names go around the explicit tracks (an automatic repeat's as many times as it was put in). */
	names = style->row_names;
	repeat = &style->row_repeat;
	explicit_count = style->row_count;
	if (columns) {
		names = style->column_names;
		repeat = &style->column_repeat;
		explicit_count = style->column_count;
	}
	if (repeat->length > 0 && repeats > 1)
		explicit_count += (repeats - 1) * repeat->length;

	/* Every explicit and implicit track is serialized independently in logical order. */
	tracks = sizes.items;
	for (index = 0; index <= sizes.count; index++) {
		/* The names of the line before the track (or after the last one). */
		line = (int)index - leading;
		named.count = 0;
		if (names != NULL && line >= 0 && line <= explicit_count)
			computed_line_names(names, repeat, repeats, line, &named);
		if (named.count > 0) {
			if (index != 0) {
				error = computed_ascii(" ", out);
				if (error != 0)
					goto cleanup;
			}
			error = css_line_names_serialize(&named, out);
			if (error != 0)
				goto cleanup;
			if (index == sizes.count)
				break;
			error = computed_ascii(" ", out);
			if (error != 0)
				goto cleanup;
		} else if (index == sizes.count) {
			break;
		} else if (index != 0) {
			/* Separate adjacent tracks without inventing any extra gap track. */
			error = computed_ascii(" ", out);
			if (error != 0)
				goto cleanup;
		}

		/* Pixel serialization uses the actual solver output, including fractional lengths. */
		error = computed_pixels(tracks[index], out);
		if (error != 0)
			goto cleanup;
	}

cleanup:
	/* Copied numeric values require no VM roots and survive no longer than this query. */
	wb_vector_release(&sizes);

	/* Allocation, query and serialization failures propagate after native storage release. */
	if (error != 0)
		return error;

	/* Succeeded: handled distinguishes a complete used value from computed fallback. */
	return 0;
}

/*
 * Writes a property's resolved value: the used value of the sizes, the
 * margins and the paddings of an element with a box, and the computed
 * value of the others.
 */
static int
computed_write_used(
	const struct css_style *style,
	const struct bind_box *box,
	int found,
	int property,
	struct wb_units *out)
{
	double width;
	double height;
	double value;
	int has_used;
	int used;
	int error;

	/* A block's (or a replaced or atomic box's) sizes are used ones; an inline box's are not. */
	used = 0;
	if (found && box->block && style->display != CSS_DISPLAY_NONE)
		used = 1;

	/* The content box's, or with border-box sizing the border box's. */
	width = box->width;
	height = box->height;
	if (style->box_sizing != CSS_BOX_SIZING_BORDER) {
		width -= box->border_left + box->border_right + box->padding_left + box->padding_right;
		height -= box->border_top + box->border_bottom + box->padding_top + box->padding_bottom;
	}

	/* The used value of the properties that have one. */
	value = 0.0;
	has_used = 0;
	switch (property) {
	case COMPUTED_WIDTH:
		has_used = used;
		value = width;
		break;
	case COMPUTED_HEIGHT:
		has_used = used;
		value = height;
		break;
	case COMPUTED_MIN_WIDTH:
		/* Box absence resolves auto independently of unavailable ordinary bounds. */
		if (box->no_generated_box && style->min_width.unit == CSS_UNIT_AUTO)
			has_used = 1;
		break;
	case COMPUTED_MIN_HEIGHT:
		/* The same proof applies to the block-axis automatic minimum. */
		if (box->no_generated_box && style->min_height.unit == CSS_UNIT_AUTO)
			has_used = 1;
		break;
	case COMPUTED_TOP:
	case COMPUTED_RIGHT:
	case COMPUTED_BOTTOM:
	case COMPUTED_LEFT:
		/* A box out of the flow's insets are its used ones. */
		has_used = found && box->inset_known &&
		    (style->position == CSS_POSITION_ABSOLUTE || style->position == CSS_POSITION_FIXED);
		value = box->inset[property - COMPUTED_TOP];
		break;
	case COMPUTED_MARGIN_TOP:
		has_used = found;
		value = box->margin_top;
		break;
	case COMPUTED_MARGIN_RIGHT:
		has_used = found;
		value = box->margin_right;
		break;
	case COMPUTED_MARGIN_BOTTOM:
		has_used = found;
		value = box->margin_bottom;
		break;
	case COMPUTED_MARGIN_LEFT:
		has_used = found;
		value = box->margin_left;
		break;
	case COMPUTED_PADDING_TOP:
		has_used = found;
		value = box->padding_top;
		break;
	case COMPUTED_PADDING_RIGHT:
		has_used = found;
		value = box->padding_right;
		break;
	case COMPUTED_PADDING_BOTTOM:
		has_used = found;
		value = box->padding_bottom;
		break;
	case COMPUTED_PADDING_LEFT:
		has_used = found;
		value = box->padding_left;
		break;
	default:
		break;
	}

	/* A used value, in pixels. */
	if (has_used) {
		error = computed_pixels(value, out);
		if (error != 0)
			return error;

		/* Succeeded: the resolved pixel value is complete. */
		return 0;
	}

	/* Every other property's computed value. */
	error = computed_write_keyword(style, property, out);
	if (error != 0)
		return error;

	/* Succeeded: the value is written. */
	return 0;
}

/* Writes a property's computed value (the ones that do not depend on the layout). */
static int
computed_write_keyword(
	const struct css_style *style,
	int property,
	struct wb_units *out)
{
	int error;

	/* Chooses by the property. */
	switch (property) {
	case COMPUTED_DISPLAY:
		error = computed_name(computed_displays, sizeof(computed_displays) / sizeof(computed_displays[0]), style->display, out);
		break;
	case COMPUTED_POSITION:
		error = computed_name(computed_positions, sizeof(computed_positions) / sizeof(computed_positions[0]), style->position, out);
		break;
	case COMPUTED_FLOAT:
		error = computed_name(computed_floats, 3, style->float_side, out);
		break;
	case COMPUTED_CLEAR:
		error = computed_name(computed_floats, 4, style->clear, out);
		break;
	case COMPUTED_VISIBILITY:
		error = computed_name(computed_visibilities, 2, style->visibility, out);
		break;
	case COMPUTED_OVERFLOW:
		error = computed_write_overflow(style, out);
		break;
	case COMPUTED_OVERFLOW_X:
		error = computed_name(computed_overflows, 5, computed_overflow_axis(style->overflow_x, style->overflow_y), out);
		break;
	case COMPUTED_OVERFLOW_Y:
		error = computed_name(computed_overflows, 5, computed_overflow_axis(style->overflow_y, style->overflow_x), out);
		break;
	case COMPUTED_OVERFLOW_INLINE:
		/* The inline axis is the horizontal one in a horizontal writing mode. */
		if (style->writing_mode == CSS_WRITING_HORIZONTAL_TB)
			error = computed_name(computed_overflows, 5, computed_overflow_axis(style->overflow_x, style->overflow_y), out);
		else
			error = computed_name(computed_overflows, 5, computed_overflow_axis(style->overflow_y, style->overflow_x), out);
		break;
	case COMPUTED_OVERFLOW_BLOCK:
		if (style->writing_mode == CSS_WRITING_HORIZONTAL_TB)
			error = computed_name(computed_overflows, 5, computed_overflow_axis(style->overflow_y, style->overflow_x), out);
		else
			error = computed_name(computed_overflows, 5, computed_overflow_axis(style->overflow_x, style->overflow_y), out);
		break;
	case COMPUTED_BOX_SIZING:
		error = computed_name(computed_box_sizings, 2, style->box_sizing, out);
		break;
	case COMPUTED_WIDTH:
		error = computed_length(&style->width, out);
		break;
	case COMPUTED_HEIGHT:
		error = computed_length(&style->height, out);
		break;
	case COMPUTED_MARGIN_TOP:
	case COMPUTED_MARGIN_RIGHT:
	case COMPUTED_MARGIN_BOTTOM:
	case COMPUTED_MARGIN_LEFT:
		error = computed_length(&style->margin[property - COMPUTED_MARGIN_TOP], out);
		break;
	case COMPUTED_PADDING_TOP:
	case COMPUTED_PADDING_RIGHT:
	case COMPUTED_PADDING_BOTTOM:
	case COMPUTED_PADDING_LEFT:
		error = computed_length(&style->padding[property - COMPUTED_PADDING_TOP], out);
		break;
	case COMPUTED_MIN_WIDTH:
		error = computed_length(&style->min_width, out);
		break;
	case COMPUTED_MIN_HEIGHT:
		error = computed_length(&style->min_height, out);
		break;
	case COMPUTED_MAX_WIDTH:
		error = computed_length(&style->max_width, out);
		break;
	case COMPUTED_MAX_HEIGHT:
		error = computed_length(&style->max_height, out);
		break;
	case COMPUTED_TOP:
	case COMPUTED_RIGHT:
	case COMPUTED_BOTTOM:
	case COMPUTED_LEFT:
		error = computed_write_offset(style, property - COMPUTED_TOP, out);
		break;
	case COMPUTED_BORDER_TOP_WIDTH:
	case COMPUTED_BORDER_RIGHT_WIDTH:
	case COMPUTED_BORDER_BOTTOM_WIDTH:
	case COMPUTED_BORDER_LEFT_WIDTH:
		error = computed_pixels(style->border_width[property - COMPUTED_BORDER_TOP_WIDTH], out);
		break;
	case COMPUTED_BORDER_TOP_STYLE:
	case COMPUTED_BORDER_RIGHT_STYLE:
	case COMPUTED_BORDER_BOTTOM_STYLE:
	case COMPUTED_BORDER_LEFT_STYLE:
		error = computed_name(computed_border_styles, 7, style->border_style[property - COMPUTED_BORDER_TOP_STYLE], out);
		break;
	case COMPUTED_BORDER_TOP_COLOR:
	case COMPUTED_BORDER_RIGHT_COLOR:
	case COMPUTED_BORDER_BOTTOM_COLOR:
	case COMPUTED_BORDER_LEFT_COLOR:
		error = computed_color(style->border_color[property - COMPUTED_BORDER_TOP_COLOR], out);
		break;
	case COMPUTED_COLOR:
		error = computed_color(style->color, out);
		break;
	case COMPUTED_BACKGROUND_COLOR:
		error = computed_color(style->background_color, out);
		break;
	case COMPUTED_BACKGROUND_IMAGE:
		/* None, or the URL in quotes. */
		if (style->background_image == NULL) {
			error = computed_ascii("none", out);
		} else {
			error = computed_write_url(style->background_image, out);
		}

		break;
	case COMPUTED_OPACITY:
		error = computed_number(style->opacity, out);
		break;
	case COMPUTED_TEXT_SHADOW:
		error = computed_text_shadow(style, out);
		break;
	case COMPUTED_Z_INDEX:
		/* Auto, or the level. */
		if (style->z_index_auto) {
			error = computed_ascii("auto", out);
		} else {
			error = computed_number(style->z_index, out);
		}

		break;
	case COMPUTED_FONT_SIZE:
		error = computed_pixels(style->font_size, out);
		break;
	case COMPUTED_FONT_WEIGHT:
		error = computed_number(style->font_weight, out);
		break;
	case COMPUTED_FONT_STYLE:
		/* Italic, or normal. */
		if (style->font_italic) {
			error = computed_ascii("italic", out);
		} else {
			error = computed_ascii("normal", out);
		}

		break;
	case COMPUTED_FONT_FAMILY:
		error = computed_write_font_family(style, out);
		break;
	case COMPUTED_LINE_HEIGHT:
		/* A number is that many times the font size. */
		if (style->line_height.unit == CSS_UNIT_NUMBER) {
			error = computed_pixels(style->line_height.value * style->font_size, out);
		} else {
			error = computed_length(&style->line_height, out);
		}

		break;
	case COMPUTED_TEXT_ALIGN:
		error = computed_name(computed_text_aligns, 6, style->text_align, out);
		break;
	case COMPUTED_TEXT_INDENT:
		error = computed_length(&style->text_indent, out);
		break;
	case COMPUTED_DIRECTION:
		error = computed_name(computed_directions, 2, style->direction, out);
		break;
	case COMPUTED_WRITING_MODE:
		error = style->writing_sideways != 0 ? computed_ascii(style->writing_sideways == 1 ? "sideways-lr" : "sideways-rl", out) : computed_name(computed_writing_modes, 3, style->writing_mode, out);
		break;
	case COMPUTED_TEXT_ORIENTATION:
		error = computed_name(computed_text_orientations, 3, style->text_orientation, out);
		break;
	case COMPUTED_CURSOR:
		error = computed_name(computed_cursors, sizeof(computed_cursors) / sizeof(computed_cursors[0]), style->cursor, out);
		break;
	case COMPUTED_TEXT_TRANSFORM:
		/* Serialize the actual cascaded keyword in CSS and camel-case accessors alike. */
		error = computed_name(computed_text_transforms, 4, style->text_transform, out);
		break;
	case COMPUTED_TEXT_DECORATION:
		error = computed_decoration(style, out);
		break;
	case COMPUTED_TEXT_DECORATION_LINE:
		error = css_decoration_line_serialize(style->text_decoration_line, out);
		break;
	case COMPUTED_TEXT_DECORATION_STYLE:
		error = computed_name(computed_decoration_styles, 5, style->text_decoration_style, out);
		break;
	case COMPUTED_TEXT_DECORATION_SKIP_INK:
		error = computed_name(computed_decoration_skip_inks, 3, style->text_decoration_skip_ink, out);
		break;
	case COMPUTED_TEXT_DECORATION_COLOR:
		if (style->text_decoration_current_color)
			error = computed_color(style->color, out);
		else
			error = computed_color(style->text_decoration_color, out);
		break;
	case COMPUTED_WHITE_SPACE:
		error = computed_name(computed_white_spaces, 5, style->white_space, out);
		break;
	case COMPUTED_VERTICAL_ALIGN:
		/* A length, or a keyword. */
		if (style->vertical_align == CSS_VALIGN_LENGTH) {
			error = computed_length(&style->vertical_offset, out);
		} else {
			error = computed_name(computed_vertical_aligns, 8, style->vertical_align, out);
		}

		break;
	case COMPUTED_LIST_STYLE_TYPE:
		error = computed_name(computed_list_styles, 5, style->list_style, out);
		break;
	case COMPUTED_BORDER_COLLAPSE:
		error = computed_name(computed_collapses, 2, style->border_collapse, out);
		break;
	case COMPUTED_CAPTION_SIDE:
		error = computed_name(computed_caption_sides, 2, style->caption_side, out);
		break;
	case COMPUTED_FLEX_DIRECTION:
		error = computed_name(computed_flex_directions, 4, style->flex_direction, out);
		break;
	case COMPUTED_FLEX_WRAP:
		error = computed_name(computed_flex_wraps, 3, style->flex_wrap, out);
		break;
	case COMPUTED_FLEX_GROW:
		error = computed_number(style->flex_grow, out);
		break;
	case COMPUTED_FLEX_SHRINK:
		error = computed_number(style->flex_shrink, out);
		break;
	case COMPUTED_FLEX_BASIS:
		error = computed_length(&style->flex_basis, out);
		break;
	case COMPUTED_FLEX:
		error = computed_number(style->flex_grow, out);
		if (error == 0)
			error = computed_ascii(" ", out);
		if (error == 0)
			error = computed_number(style->flex_shrink, out);
		if (error == 0)
			error = computed_ascii(" ", out);
		if (error == 0)
			error = computed_length(&style->flex_basis, out);
		break;
	case COMPUTED_ANCHOR_NAME:
		error = computed_anchor_names(style->anchor_names, "none", out);
		break;
	case COMPUTED_ANCHOR_SCOPE:
		if (style->anchor_scope_all)
			error = computed_ascii("all", out);
		else
			error = computed_anchor_names(style->anchor_scope, "none", out);
		break;
	case COMPUTED_POSITION_ANCHOR:
		if (style->position_anchor == NULL)
			error = computed_ascii("auto", out);
		else
			error = css_serialize_ident(style->position_anchor, out);
		break;
	case COMPUTED_POSITION_AREA:
		error = css_position_area_serialize(style->position_area, out);
		break;
	case COMPUTED_POSITION_TRY_FALLBACKS:
		error = css_try_fallbacks_serialize(style->position_try, out);
		break;
	case COMPUTED_POSITION_TRY_ORDER:
		error = css_try_order_serialize(style->position_try_order, out);
		break;
	case COMPUTED_POSITION_TRY:
		error = 0;
		if (style->position_try_order != 0) {
			error = css_try_order_serialize(style->position_try_order, out);
			if (error == 0)
				error = computed_ascii(" ", out);
		}
		if (error == 0)
			error = css_try_fallbacks_serialize(style->position_try, out);
		break;
	case COMPUTED_POSITION_VISIBILITY:
		error = css_position_visibility_serialize(style->position_visibility, out);
		break;
	case COMPUTED_ZOOM:
		error = computed_number(style->zoom, out);
		break;
	case COMPUTED_VIEW_TRANSITION_NAME:
		if (style->view_transition_name == NULL)
			error = computed_ascii("none", out);
		else
			error = vm_string_append_units(style->view_transition_name, out);
		break;
	case COMPUTED_VIEW_TRANSITION_CLASS:
		if (style->view_transition_class == NULL)
			error = computed_ascii("none", out);
		else
			error = vm_string_append_units(style->view_transition_class, out);
		break;
	case COMPUTED_ANIMATION_NAME:
		if (style->animation_name == NULL)
			error = computed_ascii("none", out);
		else
			error = vm_string_append_units(style->animation_name, out);
		break;
	case COMPUTED_ANIMATION_DURATION:
	case COMPUTED_ANIMATION_DELAY:
		error = computed_number(property == COMPUTED_ANIMATION_DURATION ? style->animation_duration : style->animation_delay, out);
		if (error == 0)
			error = computed_ascii("s", out);
		break;
	case COMPUTED_ANIMATION_PLAY_STATE:
		error = computed_ascii(style->animation_paused ? "paused" : "running", out);
		break;
	case COMPUTED_ANIMATION_TIMING_FUNCTION:
		if (style->animation_timing == NULL)
			error = computed_ascii("ease", out);
		else
			error = vm_string_append_units(style->animation_timing, out);
		break;
	case COMPUTED_MIX_BLEND_MODE:
		error = computed_ascii(style->mix_blend_mode == 1 ? "plus-lighter" : "normal", out);
		break;
	case COMPUTED_FLEX_FLOW:
		error = computed_name(computed_flex_directions, 4, style->flex_direction, out);
		if (error == 0)
			error = computed_ascii(" ", out);
		if (error == 0)
			error = computed_name(computed_flex_wraps, 3, style->flex_wrap, out);
		break;
	case COMPUTED_JUSTIFY_CONTENT:
		error = computed_align(style->justify_content == CSS_ALIGN_STRETCH ? CSS_ALIGN_NORMAL : style->justify_content, (style->flex_keywords & 1u) != 0, out);
		break;
	case COMPUTED_JUSTIFY_ITEMS:
		error = computed_name(computed_justifies, 14, style->justify_items, out);
		break;
	case COMPUTED_ALIGN_ITEMS:
		if (style->last_baselines & 4u)
			error = computed_ascii("last baseline", out);
		else
			error = computed_align(style->align_items, (style->flex_keywords & 4u) != 0, out);
		break;
	case COMPUTED_ALIGN_SELF:
		if (style->last_baselines & 8u)
			error = computed_ascii("last baseline", out);
		else if (style->align_self == CSS_ALIGN_SELF_START)
			error = computed_ascii("self-start", out);
		else if (style->align_self == CSS_ALIGN_SELF_END)
			error = computed_ascii("self-end", out);
		else
			error = computed_align(style->align_self, (style->flex_keywords & 8u) != 0, out);
		break;
	case COMPUTED_JUSTIFY_SELF:
		if (style->last_baselines & 0x100u)
			error = computed_ascii("last baseline", out);
		else if (style->justify_self == CSS_ALIGN_ANCHOR_CENTER)
			error = computed_ascii("anchor-center", out);
		else
			error = computed_name(computed_justifies, 14, style->justify_self, out);
		break;
	case COMPUTED_ALIGN_CONTENT:
		error = computed_align(style->align_content == CSS_ALIGN_STRETCH ? CSS_ALIGN_NORMAL : style->align_content, (style->flex_keywords & 2u) != 0, out);
		break;
	case COMPUTED_ORDER:
		error = computed_number(style->order, out);
		break;
	case COMPUTED_ROW_GAP:
		error = computed_length(&style->row_gap, out);
		break;
	case COMPUTED_COLUMN_GAP:
		error = computed_length(&style->column_gap, out);
		break;
	case COMPUTED_OUTLINE_WIDTH:
		error = computed_pixels(style->outline_width, out);
		break;
	case COMPUTED_OUTLINE_STYLE:
		error = computed_name(computed_border_styles, 7, style->outline_style, out);
		break;
	case COMPUTED_OUTLINE_COLOR:
		error = computed_color(style->outline_color, out);
		break;
	case COMPUTED_GRID_TEMPLATE_COLUMNS:
		error = computed_write_tracks(style->columns, style->column_count, style->column_names, out);
		break;
	case COMPUTED_GRID_TEMPLATE_ROWS:
		error = computed_write_tracks(style->rows, style->row_count, style->row_names, out);
		break;
	case COMPUTED_GRID_AUTO_FLOW:
		if (style->grid_auto_flow & CSS_GRID_FLOW_COLUMN) {
			if (style->grid_auto_flow & CSS_GRID_FLOW_DENSE)
				error = computed_ascii("column dense", out);
			else
				error = computed_ascii("column", out);
		} else if (style->grid_auto_flow & CSS_GRID_FLOW_DENSE) {
			error = computed_ascii("dense", out);
		} else {
			error = computed_ascii("row", out);
		}
		break;
	case COMPUTED_GRID_AUTO_COLUMNS:
		error = computed_write_tracks(style->auto_columns, style->auto_column_count, NULL, out);
		break;
	case COMPUTED_GRID_AUTO_ROWS:
		error = computed_write_tracks(style->auto_rows, style->auto_row_count, NULL, out);
		break;
	case COMPUTED_GRID_TEMPLATE_AREAS:
		error = css_grid_areas_serialize(style->areas, out);
		break;
	case COMPUTED_GRID_ROW_START:
		error = css_grid_line_serialize(&style->grid_row, 0, out);
		break;
	case COMPUTED_GRID_ROW_END:
		error = css_grid_line_serialize(&style->grid_row, 1, out);
		break;
	case COMPUTED_GRID_COLUMN_START:
		error = css_grid_line_serialize(&style->grid_column, 0, out);
		break;
	case COMPUTED_GRID_COLUMN_END:
		error = css_grid_line_serialize(&style->grid_column, 1, out);
		break;
	case COMPUTED_GRID_ROW:
		error = css_grid_lines_serialize(&style->grid_row, out);
		break;
	case COMPUTED_GRID_COLUMN:
		error = css_grid_lines_serialize(&style->grid_column, out);
		break;
	case COMPUTED_GRID_AREA:
		error = css_grid_area_serialize(&style->grid_row, &style->grid_column, out);
		break;
	default:
		/* transform: the engine has none. */
		error = computed_ascii("none", out);
		break;
	}

	/* Reports the writing. */
	return error;
}

/*
 * Writes an inset (top, right, bottom or left: side 0 to 3): a relatively
 * positioned box's auto one is the opposite one's negation in pixels, as
 * the box moves by it; any other is its computed value.
 */
static int
computed_write_offset(
	const struct css_style *style,
	int side,
	struct wb_units *out)
{
	const struct css_length *length;
	const struct css_length *opposite;
	int error;

	/* The side and the one across from it. */
	length = &style->offset[side];
	opposite = &style->offset[(side + 2) % 4];

	/* A relative box's auto side against a side in pixels. */
	if (style->position == CSS_POSITION_RELATIVE &&
	    length->unit == CSS_UNIT_AUTO &&
	    opposite->unit == CSS_UNIT_PX) {
		error = computed_pixels(-opposite->value, out);
		return error;
	}

	/* A relative box with both auto has not moved. */
	if (style->position == CSS_POSITION_RELATIVE &&
	    length->unit == CSS_UNIT_AUTO &&
	    opposite->unit == CSS_UNIT_AUTO) {
		error = computed_pixels(0.0, out);
		return error;
	}

	/* Succeeded: the computed value. */
	error = computed_length(length, out);
	return error;
}

/*
 * Writes font-family: each family as the style names it, a name with a
 * character other than a letter, a digit or a hyphen in double quotes,
 * separated by ", ".
 */
static int
computed_write_font_family(
	const struct css_style *style,
	struct wb_units *out)
{
	const struct vm_string *family;
	size_t index;
	int quoted;
	int error;

	/* Each family. */
	for (index = 0; index < (size_t)style->family_count; index++) {
		family = style->families[index];

		/* The separator before all but the first. */
		if (index > 0) {
			error = computed_ascii(", ", out);
			if (error != 0)
				return error;
		}

		/* The name, in quotes when it needs them. */
		quoted = computed_needs_quotes(family);
		if (quoted) {
			error = computed_write_url_quoted(family, out);
		} else {
			error = vm_string_append_units(family, out);
		}

		/* A failed name. */
		if (error != 0)
			return error;
	}

	/* Succeeded: the families are written. */
	return 0;
}

/* Writes anchor names a comma apart (none when there are none). */
static int
computed_anchor_names(
	const struct css_anchor_names *names,
	const char *none,
	struct wb_units *out)
{
	int index;
	int error;

	/* None. */
	if (names == NULL || names->count == 0)
		return computed_ascii(none, out);

	/* Each name. */
	for (index = 0; index < names->count; index++) {
		if (index > 0) {
			error = computed_ascii(", ", out);
			if (error != 0)
				return error;
		}
		error = css_serialize_ident(names->names[index], out);
		if (error != 0)
			return error;
	}
	return 0;
}

/*
 * Finds the names of a line of an explicit grid whose automatic repeat
 * was put in repeats times: the template's own names, with the repeat's
 * first and last lines' own names meeting at each line between two
 * repetitions.
 */
static void
computed_line_names(
	const struct css_line_names *names,
	const struct css_track_repeat *repeat,
	int repeats,
	int line,
	struct css_line_names *out)
{
	int at;
	int length;
	int offset;

	/* Without a repeat the template's line is the line. */
	out->count = 0;
	at = repeat->at;
	length = repeat->length;
	if (length <= 0 || repeats <= 1) {
		computed_names_add(out, &names[line], 0, names[line].count);
		return;
	}

	/* Before the repeat, at its first line, and after it. */
	if (line <= at) {
		computed_names_add(out, &names[line], 0, names[line].count);
		return;
	}
	offset = line - at;
	if (offset >= repeats * length) {
		line -= (repeats - 1) * length;
		computed_names_add(out, &names[line], 0, names[line].count);
		return;
	}

	/* Between two repetitions: the last line's own names, then the first line's own ones. */
	if (offset % length == 0) {
		computed_names_add(out, &names[at + length], 0, repeat->name_end);
		computed_names_add(out, &names[at], repeat->name_prefix, names[at].count);
		return;
	}

	/* A line inside a repetition. */
	line = at + offset % length;
	computed_names_add(out, &names[line], 0, names[line].count);
}

/* Adds the names first to end of a line to a list (as many as it holds). */
static void
computed_names_add(
	struct css_line_names *out,
	const struct css_line_names *from,
	int first,
	int end)
{
	int index;

	/* Each name in turn. */
	for (index = first; index < end && out->count < CSS_LINE_NAMES; index++)
		out->names[out->count++] = from->names[index];
}

/* Writes computed track breadths when actual used tracks are not requested or available. */
static int
computed_write_tracks(
	const struct css_track *tracks,
	int count,
	const struct css_line_names *names,
	struct wb_units *out)
{
	const struct css_track *track;
	int index;
	int bounded;
	int error;

	/* An absent explicit track list has the computed value none. */
	if (count == 0) {
		error = computed_ascii("none", out);
		if (error != 0)
			return error;

		/* Succeeded: the empty template is serialized. */
		return 0;
	}

	/* Separates every computed track without dropping its intrinsic bounds. */
	for (index = 0; index < count; index++) {
		/* Resolves the current scalar track before appending its separator. */
		track = &tracks[index];
		if (index > 0) {
			error = computed_ascii(" ", out);
			if (error != 0)
				return error;
		}

		/* The names of the line before the track. */
		if (names != NULL && names[index].count > 0) {
			error = css_line_names_serialize(&names[index], out);
			if (error != 0)
				return error;
			error = computed_ascii(" ", out);
			if (error != 0)
				return error;
		}

		/* Explicit bounds retain minmax except for the canonical auto-minimum flex breadth. */
		bounded = 0;
		if (track->minimum_rule != CSS_TRACK_MINIMUM_NONE)
			bounded = 1;

		/* An ordinary flexible breadth already implies the same automatic minimum. */
		if (track->minimum_rule == CSS_TRACK_MINIMUM_AUTO && track->kind == CSS_TRACK_FR)
			bounded = 0;

		/* Opens the explicit function only when its lower breadth must be serialized. */
		if (bounded) {
			error = computed_ascii("minmax(", out);
			if (error != 0)
				return error;

			/* Writes the accepted minimum independently of its maximum. */
			error = computed_track_minimum(track, out);
			if (error != 0)
				return error;

			/* Separates the complete lower breadth from the upper breadth. */
			error = computed_ascii(", ", out);
			if (error != 0)
				return error;
		}

		/* Writes either the plain breadth or the maximum of minmax. */
		error = computed_track_breadth(track, out);
		if (error != 0)
			return error;

		/* Closes only the explicit function that this track opened. */
		if (bounded) {
			error = computed_ascii(")", out);
			if (error != 0)
				return error;
		}
	}

	/* The names of the line after the last track. */
	if (names != NULL && names[count].count > 0) {
		error = computed_ascii(" ", out);
		if (error != 0)
			return error;
		error = css_line_names_serialize(&names[count], out);
		if (error != 0)
			return error;
	}

	/* Succeeded: all computed track functions and keywords are complete. */
	return 0;
}

/* Writes the lower bound whose explicit minmax rule survived the cascade. */
static int
computed_track_minimum(
	const struct css_track *track,
	struct wb_units *out)
{
	const char *keyword;
	int error;

	/* Length minima retain percentages and calculated offsets in their computed form. */
	if (track->minimum_rule == CSS_TRACK_MINIMUM_LENGTH) {
		error = computed_length(&track->minimum, out);
		if (error != 0)
			return error;

		/* Succeeded: the computed lower length is complete. */
		return 0;
	}

	/* Selects the distinct accepted intrinsic minimum without consulting layout. */
	keyword = "auto";
	if (track->minimum_rule == CSS_TRACK_MINIMUM_MIN_CONTENT) {
		keyword = "min-content";
	} else if (track->minimum_rule == CSS_TRACK_MINIMUM_MAX_CONTENT) {
		keyword = "max-content";
	}

	/* Appends the complete minimum keyword. */
	error = computed_ascii(keyword, out);
	if (error != 0)
		return error;

	/* Succeeded: the intrinsic lower breadth is complete. */
	return 0;
}

/* Writes a plain track size or the independent upper breadth of minmax. */
static int
computed_track_breadth(
	const struct css_track *track,
	struct wb_units *out)
{
	const struct css_length *length;
	const char *keyword;
	int error;

	/* Fit-content retains its resolved limit without substituting a native used track size. */
	if (track->maximum_rule == CSS_TRACK_MAXIMUM_FIT_CONTENT) {
		error = computed_ascii("fit-content(", out);
		if (error != 0)
			return error;

		/* Appends the computed length-percentage inside the function. */
		error = computed_length(&track->size, out);
		if (error != 0)
			return error;

		/* Completes the fit-content function after its bound. */
		error = computed_ascii(")", out);
		if (error != 0)
			return error;

		/* Succeeded: the complete computed fit bound is written. */
		return 0;
	}

	/* Flexible breadths retain their original factor and fr unit. */
	if (track->kind == CSS_TRACK_FR) {
		error = computed_number(track->fr, out);
		if (error != 0)
			return error;

		/* Completes the flexible dimension after its number. */
		error = computed_ascii("fr", out);
		if (error != 0)
			return error;

		/* Succeeded: the flexible breadth is complete. */
		return 0;
	}

	/* Fixed breadths retain the cascade's ordinary length serialization. */
	if (track->kind == CSS_TRACK_LENGTH) {
		/* Explicit length minima preserve the independent computed maximum. */
		length = &track->size;
		if (track->minimum_rule == CSS_TRACK_MINIMUM_LENGTH)
			length = &track->computed_maximum;

		/* Serializes the computed upper bound without substituting its used floor. */
		error = computed_length(length, out);
		if (error != 0)
			return error;

		/* Succeeded: the computed fixed breadth is complete. */
		return 0;
	}

	/* Selects the retained intrinsic maximum instead of collapsing it to auto. */
	keyword = "auto";
	if (track->maximum_rule == CSS_TRACK_MAXIMUM_MIN_CONTENT) {
		keyword = "min-content";
	} else if (track->maximum_rule == CSS_TRACK_MAXIMUM_MAX_CONTENT) {
		keyword = "max-content";
	}

	/* Appends the complete intrinsic maximum keyword. */
	error = computed_ascii(keyword, out);
	if (error != 0)
		return error;

	/* Succeeded: the intrinsic upper breadth is complete. */
	return 0;
}

/* The computed overflow of an axis: visible becomes auto when the other axis scrolls or hides. */
static int
computed_overflow_axis(
	int own,
	int other)
{
	int other_open;

	/* The other axis shows everything (visible or clip). */
	other_open = other == CSS_OVERFLOW_VISIBLE || other == CSS_OVERFLOW_CLIP;
	if (own == CSS_OVERFLOW_VISIBLE && !other_open)
		return CSS_OVERFLOW_AUTO;
	return own;
}

/*
 * Maps a flow-relative property (margin-inline-start and the like) to the
 * physical property it stands for in the style's writing mode and direction.
 */
static int
computed_logical_property(
	const struct css_style *style,
	int property)
{
	static const int bases[6] = {
		COMPUTED_MARGIN_TOP, COMPUTED_PADDING_TOP, COMPUTED_TOP,
		COMPUTED_BORDER_TOP_WIDTH, COMPUTED_BORDER_TOP_STYLE, COMPUTED_BORDER_TOP_COLOR
	};
	int index;
	int family;
	int part;
	int axis_block;
	int end;
	int side;
	int vertical;
	int reversed;

	/* Which family, and which of its four sides. */
	index = property - COMPUTED_LOGICAL_FIRST;
	family = index / 4;
	part = index % 4;
	axis_block = part >= 2;
	end = part % 2;
	vertical = style->writing_mode != CSS_WRITING_HORIZONTAL_TB;

	/* The block axis runs top to bottom, or from the right (vertical-rl) or the left (vertical-lr). */
	if (axis_block) {
		if (!vertical)
			side = end ? 2 : 0;
		else if (style->writing_mode == CSS_WRITING_VERTICAL_RL)
			side = end ? 3 : 1;
		else
			side = end ? 1 : 3;
		return bases[family] + side;
	}

	/* The inline axis runs left to right (right to left for rtl), or top to bottom (bottom to top for sideways-lr). */
	reversed = style->direction == CSS_DIRECTION_RTL;
	if (vertical && style->writing_sideways == 1)
		reversed = !reversed;
	if (reversed)
		end = !end;
	if (!vertical)
		side = end ? 1 : 3;
	else
		side = end ? 2 : 0;
	return bases[family] + side;
}

/* Writes overflow: one keyword when the two axes agree, otherwise both. */
static int
computed_write_overflow(
	const struct css_style *style,
	struct wb_units *out)
{
	int across;
	int down;
	int error;

	/* The horizontal axis's. */
	across = computed_overflow_axis(style->overflow_x, style->overflow_y);
	down = computed_overflow_axis(style->overflow_y, style->overflow_x);
	error = computed_name(computed_overflows, 5, across, out);
	if (error != 0)
		return error;

	/* The same on both is one keyword. */
	if (down == across)
		return 0;

	/* Succeeded: the vertical axis's after a space. */
	error = computed_ascii(" ", out);
	if (error != 0)
		return error;
	error = computed_name(computed_overflows, 5, down, out);
	return error;
}

/* Writes a URL as url("...") (background-image). */
static int
computed_write_url(
	const struct vm_string *url,
	struct wb_units *out)
{
	int error;

	/* The function around the quoted URL. */
	error = computed_ascii("url(", out);
	if (error != 0)
		return error;
	error = computed_write_url_quoted(url, out);
	if (error != 0)
		return error;
	error = computed_ascii(")", out);
	return error;
}

/* Writes a string in double quotes. */
static int
computed_write_url_quoted(
	const struct vm_string *text,
	struct wb_units *out)
{
	int error;

	/* The quotes around the string. */
	error = computed_ascii("\"", out);
	if (error != 0)
		return error;
	error = vm_string_append_units(text, out);
	if (error != 0)
		return error;
	error = computed_ascii("\"", out);
	return error;
}

/* Tells whether a font family's name is written in quotes: it has a character other than a letter, a digit or a hyphen. */
static int
computed_needs_quotes(
	const struct vm_string *family)
{
	size_t index;
	uint16_t unit;

	/* Each character. */
	for (index = 0; index < family->length; index++) {
		unit = vm_string_at(family, index);
		if (unit >= 'a' && unit <= 'z')
			continue;
		if (unit >= 'A' && unit <= 'Z')
			continue;
		if (unit >= '0' && unit <= '9')
			continue;
		if (unit == '-')
			continue;
		return 1;
	}

	/* Every character is plain. */
	return 0;
}

/* Writes a percentage, in calc() with the pixels it has added. */
static int
computed_write_percent(
	const struct css_length *length,
	struct wb_units *out)
{
	const char *sign;
	int error;

	/* A plain percentage. */
	if (length->offset == 0.0f) {
		error = computed_number(length->value, out);
		if (error != 0)
			return error;
		error = computed_ascii("%", out);
		return error;
	}

	/* The sign between the two parts. */
	sign = "% + ";
	if (length->offset < 0.0f)
		sign = "% - ";

	/* Succeeded: calc(P% + Npx). */
	error = computed_ascii("calc(", out);
	if (error != 0)
		return error;
	error = computed_number(length->value, out);
	if (error != 0)
		return error;
	error = computed_ascii(sign, out);
	if (error != 0)
		return error;
	error = computed_pixels(fabsf(length->offset), out);
	if (error != 0)
		return error;
	error = computed_ascii(")", out);
	return error;
}

/* Writes a flex alignment keyword; start and end keep the spelling they were written in. */
static int
computed_align(
	int value,
	int flex,
	struct wb_units *out)
{
	int error;

	if (value == CSS_ALIGN_START) {
		error = computed_ascii(flex ? "flex-start" : "start", out);
		return error;
	}
	if (value == CSS_ALIGN_END) {
		error = computed_ascii(flex ? "flex-end" : "end", out);
		return error;
	}
	if (value == CSS_ALIGN_ANCHOR_CENTER)
		return computed_ascii("anchor-center", out);
	error = computed_name(computed_aligns, 10, value, out);
	return error;
}

/* Writes the name of a keyword from a table, or nothing for a value past its end. */
static int
computed_name(
	const char *const *names,
	size_t count,
	int value,
	struct wb_units *out)
{
	int error;

	/* A value the table does not name. */
	if (value < 0 || (size_t)value >= count)
		return 0;

	/* Succeeded: its name. */
	error = computed_ascii(names[value], out);
	return error;
}

/* Writes a computed length: pixels, a percentage (with the pixels calc() added), or a keyword. */
static int
computed_length(
	const struct css_length *length,
	struct wb_units *out)
{
	int error;

	/* Serializes the computed unit without resolving percentage dependence. */
	switch (length->unit) {
	case CSS_UNIT_PX:
		error = computed_pixels(length->value, out);
		break;
	case CSS_UNIT_PERCENT:
		error = computed_write_percent(length, out);
		break;
	case CSS_UNIT_AUTO:
		error = computed_ascii("auto", out);
		break;
	case CSS_UNIT_NONE:
		error = computed_ascii("none", out);
		break;
	case CSS_UNIT_NORMAL:
		error = computed_ascii("normal", out);
		break;
	case CSS_UNIT_NUMBER:
		error = computed_number(length->value, out);
		break;
	case CSS_UNIT_MAX_CONTENT:
		error = computed_ascii("max-content", out);
		break;
	case CSS_UNIT_MIN_CONTENT:
		error = computed_ascii("min-content", out);
		break;
	case CSS_UNIT_CONTENT:
		error = computed_ascii("content", out);
		break;
	default:
		error = computed_ascii("fit-content", out);
		break;
	}

	/* Propagates a failed append to the computed string. */
	if (error != 0)
		return error;

	/* Succeeded: the complete length or keyword is appended. */
	return 0;
}

/* Appends one noninitial computed shorthand component. */
static int
computed_decoration_append(struct wb_units *out, const struct wb_units *part)
{
	int error;

	if (part->length == 0U)
		return 0;
	if (out->length != 0U) {
		error = computed_ascii(" ", out);
		if (error != 0)
			return error;
	}
	return wb_units_append(out, part->data, part->length);
}

/* Serializes the text shadow as its color and its offsets and blur in pixels, or none. */
static int
computed_text_shadow(
	const struct css_style *style,
	struct wb_units *out)
{
	const struct css_shadow *shadow;
	int error;

	/* No shadow. */
	if (!style->text_shadow_set)
		return computed_ascii("none", out);

	/* The color, then the three lengths. */
	shadow = &style->text_shadow;
	error = computed_color(shadow->color, out);
	if (error == 0)
		error = computed_ascii(" ", out);
	if (error == 0)
		error = computed_pixels(shadow->x, out);
	if (error == 0)
		error = computed_ascii(" ", out);
	if (error == 0)
		error = computed_pixels(shadow->y, out);
	if (error == 0)
		error = computed_ascii(" ", out);
	if (error == 0)
		error = computed_pixels(shadow->blur, out);
	return error;
}

/* Serializes the four cascaded decoration longhands as a computed shorthand. */
static int
computed_decoration(const struct css_style *style, struct wb_units *out)
{
	struct wb_units parts[4];
	size_t index;
	int error;

	for (index = 0U; index < 4U; index++)
		wb_units_init(&parts[index]);
	error = css_decoration_line_serialize(style->text_decoration_line, &parts[0]);
	if (error != 0)
		goto cleanup;
	if (style->text_decoration_thickness_kind == 1)
		error = computed_ascii("from-font", &parts[1]);
	else if (style->text_decoration_thickness_kind == 2)
		error = computed_length(&style->text_decoration_thickness, &parts[1]);
	if (error != 0)
		goto cleanup;
	error = computed_name(computed_decoration_styles, 5, style->text_decoration_style, &parts[2]);
	if (error != 0)
		goto cleanup;
	if (!style->text_decoration_current_color) {
		error = computed_color(style->text_decoration_color, &parts[3]);
		if (error != 0)
			goto cleanup;
	}

	/* None stands alone when every other component has its initial value. */
	if (style->text_decoration_line != 0 ||
	    (style->text_decoration_thickness_kind == 0 &&
	    style->text_decoration_style == CSS_DECORATION_SOLID &&
	    style->text_decoration_current_color)) {
		error = computed_decoration_append(out, &parts[0]);
		if (error != 0)
			goto cleanup;
	}
	if (style->text_decoration_thickness_kind != 0) {
		error = computed_decoration_append(out, &parts[1]);
		if (error != 0)
			goto cleanup;
	}
	if (style->text_decoration_style != CSS_DECORATION_SOLID) {
		error = computed_decoration_append(out, &parts[2]);
		if (error != 0)
			goto cleanup;
	}
	if (!style->text_decoration_current_color)
		error = computed_decoration_append(out, &parts[3]);

cleanup:
	for (index = 0U; index < 4U; index++)
		wb_units_release(&parts[index]);
	return error;
}

/* Writes a number of pixels ("12px", "12.5px"). */
static int
computed_pixels(
	double value,
	struct wb_units *out)
{
	int error;

	/* The number and the unit. */
	error = computed_number(value, out);
	if (error != 0)
		return error;
	error = computed_ascii("px", out);
	if (error != 0)
		return error;

	/* Succeeded: the pixels are written. */
	return 0;
}

/*
 * Writes a number as other browsers write the ones of CSS: a whole number
 * without a point, anything else with at most six significant digits and
 * no trailing zeros.
 */
static int
computed_number(
	double value,
	struct wb_units *out)
{
	char text[64];
	double whole;
	double size;

	/* Negative zero, or a number that is not one, is 0. */
	if (value == 0.0 || value != value)
		value = 0.0;

	/* A whole or a large number without a point, any other with six significant digits. */
	whole = floor(value);
	size = fabs(value);
	if (whole == value || size >= 1e6) {
		snprintf(text, sizeof(text), "%.0f", value);
	} else {
		snprintf(text, sizeof(text), "%.6g", value);
	}

	/* Succeeded: the text. */
	return computed_ascii(text, out);
}

/*
 * Writes a color as rgb(r, g, b), or rgba(r, g, b, a) when it is not
 * opaque, the alpha with the fewest decimals that stand for its byte.
 */
static int
computed_color(
	uint32_t color,
	struct wb_units *out)
{
	char text[64];
	unsigned alpha;
	unsigned back;
	double fraction;
	double rounded;
	double scale;
	int digits;

	/* An opaque color. */
	alpha = (color >> 24) & 0xffU;
	if (alpha == 0xffU) {
		snprintf(text, sizeof(text), "rgb(%u, %u, %u)", (unsigned)((color >> 16) & 0xffU), (unsigned)((color >> 8) & 0xffU),
		    (unsigned)(color & 0xffU));
		return computed_ascii(text, out);
	}

	/* The shortest fraction (up to three decimals) whose byte is the alpha's. */
	fraction = alpha / 255.0;
	rounded = fraction;
	for (digits = 1; digits <= 3; digits++) {
		scale = pow(10.0, digits);
		rounded = floor(fraction * scale + 0.5) / scale;
		back = (unsigned)floor(rounded * 255.0 + 0.5);
		if (back == alpha)
			break;
	}

	/* The fraction found, or the one of three decimals. */
	fraction = rounded;

	/* Succeeded: the color with its alpha. */
	snprintf(text, sizeof(text), "rgba(%u, %u, %u, %g)", (unsigned)((color >> 16) & 0xffU), (unsigned)((color >> 8) & 0xffU),
	    (unsigned)(color & 0xffU), fraction);
	return computed_ascii(text, out);
}

/* Appends ASCII characters. */
static int
computed_ascii(
	const char *ascii,
	struct wb_units *out)
{
	uint16_t unit;
	size_t index;
	int error;

	/* Each character. */
	for (index = 0; ascii[index] != '\0'; index++) {
		unit = (uint16_t)(unsigned char)ascii[index];
		error = wb_units_append(out, &unit, 1);
		if (error != 0)
			return error;
	}

	/* Succeeded: the characters are appended. */
	return 0;
}
