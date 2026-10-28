/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * CSS values: the property table, the parsing of a declaration's value
 * into one or more longhand declarations, colors and lengths.
 */

#include "css/internal.h"

#include <errno.h>
#include <float.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

/* The most items a content value keeps. */
#define VALUES_CONTENT_ITEMS	8

/* The widest supported shorthand expands border's four sides into twelve longhands. */
#define VALUES_EXPANSION_MAX	16

/* values_border's side for a logical side: this plus 0 to 3 (inline-start, inline-end, block-start, block-end). */
#define VALUES_LOGICAL_SIDE	4

/*
 * Where the areas form of grid-template is in a row: before it, after its
 * leading line names, after its string, after its size, or after its
 * trailing line names (the next row may still have leading ones).
 */
enum values_row_state {
	VALUES_ROW_START,
	VALUES_ROW_LEADING_NAMES,
	VALUES_ROW_STRING,
	VALUES_ROW_SIZE,
	VALUES_ROW_TRAILING_NAMES
};

/* The shorthands this pass expands (numbered after the longhands). */
enum values_shorthand {
	SHORT_MARGIN = CSS_PROP_COUNT,
	SHORT_PADDING,
	SHORT_BORDER,
	SHORT_BORDER_TOP,
	SHORT_BORDER_RIGHT,
	SHORT_BORDER_BOTTOM,
	SHORT_BORDER_LEFT,
	SHORT_BORDER_INLINE_START,
	SHORT_BORDER_INLINE_END,
	SHORT_BORDER_BLOCK_START,
	SHORT_BORDER_BLOCK_END,
	SHORT_BORDER_WIDTH,
	SHORT_BORDER_STYLE,
	SHORT_BORDER_COLOR,
	SHORT_BACKGROUND,
	SHORT_BACKGROUND_POSITION,
	SHORT_BACKGROUND_SIZE,
	SHORT_FONT,
	SHORT_TEXT_DECORATION,
	SHORT_LIST_STYLE,
	SHORT_MARGIN_INLINE,
	SHORT_MARGIN_BLOCK,
	SHORT_PADDING_INLINE,
	SHORT_PADDING_BLOCK,
	SHORT_INSET,
	SHORT_INSET_INLINE,
	SHORT_INSET_BLOCK,
	SHORT_FLEX,
	SHORT_FLEX_FLOW,
	SHORT_GAP,
	SHORT_BORDER_RADIUS,
	SHORT_RADIUS_TOP_LEFT,
	SHORT_RADIUS_TOP_RIGHT,
	SHORT_RADIUS_BOTTOM_RIGHT,
	SHORT_RADIUS_BOTTOM_LEFT,
	SHORT_OUTLINE,
	SHORT_GRID_COLUMN,
	SHORT_GRID_ROW,
	SHORT_GRID_AREA,
	SHORT_GRID_TEMPLATE,
	SHORT_GRID,
	SHORT_PLACE_ITEMS,
	SHORT_PLACE_SELF,
	SHORT_PLACE_CONTENT,
	SHORT_POSITION_TRY,
	SHORT_CONTAINER,
	SHORT_BORDER_SPACING,
	SHORT_OVERFLOW,
	SHORT_TRANSFORM,
	SHORT_TRANSFORM_ORIGIN,
	SHORT_ASPECT_RATIO,
	SHORT_OBJECT_POSITION
};

/*
 * One property name and its number (a longhand or a shorthand).
 */
struct values_name {
	const char *name;
	int property;
};

/*
 * One keyword of a property and the value it stands for.
 */
struct values_keyword {
	const char *name;
	int value;
};

/*
 * One operand of a calculation while it is read: a plain number, or a sum
 * of lengths.
 */
struct values_term {
	int is_number;
	float number;
	struct css_calc_sum sum;
};

/*
 * A named color and its RGB value.
 */
struct values_color {
	const char *name;
	uint32_t rgb;
};

/* The property names this pass knows. */
static const struct values_name values_names[] = {
	{ "display", CSS_PROP_DISPLAY },
	{ "position", CSS_PROP_POSITION },
	{ "float", CSS_PROP_FLOAT },
	{ "visibility", CSS_PROP_VISIBILITY },
	{ "width", CSS_PROP_WIDTH },
	{ "height", CSS_PROP_HEIGHT },
	{ "min-width", CSS_PROP_MIN_WIDTH },
	{ "max-width", CSS_PROP_MAX_WIDTH },
	{ "min-height", CSS_PROP_MIN_HEIGHT },
	{ "max-height", CSS_PROP_MAX_HEIGHT },
	{ "margin-top", CSS_PROP_MARGIN_TOP },
	{ "margin-right", CSS_PROP_MARGIN_RIGHT },
	{ "margin-bottom", CSS_PROP_MARGIN_BOTTOM },
	{ "margin-left", CSS_PROP_MARGIN_LEFT },
	{ "padding-top", CSS_PROP_PADDING_TOP },
	{ "padding-right", CSS_PROP_PADDING_RIGHT },
	{ "padding-bottom", CSS_PROP_PADDING_BOTTOM },
	{ "padding-left", CSS_PROP_PADDING_LEFT },
	{ "border-top-width", CSS_PROP_BORDER_TOP_WIDTH },
	{ "border-right-width", CSS_PROP_BORDER_RIGHT_WIDTH },
	{ "border-bottom-width", CSS_PROP_BORDER_BOTTOM_WIDTH },
	{ "border-left-width", CSS_PROP_BORDER_LEFT_WIDTH },
	{ "border-top-style", CSS_PROP_BORDER_TOP_STYLE },
	{ "border-right-style", CSS_PROP_BORDER_RIGHT_STYLE },
	{ "border-bottom-style", CSS_PROP_BORDER_BOTTOM_STYLE },
	{ "border-left-style", CSS_PROP_BORDER_LEFT_STYLE },
	{ "border-top-color", CSS_PROP_BORDER_TOP_COLOR },
	{ "border-right-color", CSS_PROP_BORDER_RIGHT_COLOR },
	{ "border-bottom-color", CSS_PROP_BORDER_BOTTOM_COLOR },
	{ "border-left-color", CSS_PROP_BORDER_LEFT_COLOR },
	{ "color", CSS_PROP_COLOR },
	{ "background-color", CSS_PROP_BACKGROUND_COLOR },
	{ "background-image", CSS_PROP_BACKGROUND_IMAGE },
	{ "background-repeat", CSS_PROP_BACKGROUND_REPEAT },
	{ "background-attachment", CSS_PROP_BACKGROUND_ATTACHMENT },
	{ "background-position-x", CSS_PROP_BACKGROUND_POSITION_X },
	{ "background-position-y", CSS_PROP_BACKGROUND_POSITION_Y },
	{ "font-size", CSS_PROP_FONT_SIZE },
	{ "font-weight", CSS_PROP_FONT_WEIGHT },
	{ "font-style", CSS_PROP_FONT_STYLE },
	{ "font-family", CSS_PROP_FONT_FAMILY },
	{ "line-height", CSS_PROP_LINE_HEIGHT },
	{ "text-align", CSS_PROP_TEXT_ALIGN },
	{ "text-indent", CSS_PROP_TEXT_INDENT },
	{ "white-space", CSS_PROP_WHITE_SPACE },
	{ "text-transform", CSS_PROP_TEXT_TRANSFORM },
	{ "cursor", CSS_PROP_CURSOR },
	{ "text-decoration-line", CSS_PROP_TEXT_DECORATION_LINE },
	{ "text-decoration-style", CSS_PROP_TEXT_DECORATION_STYLE },
	{ "text-decoration-skip-ink", CSS_PROP_TEXT_DECORATION_SKIP_INK },
	{ "text-decoration-color", CSS_PROP_TEXT_DECORATION_COLOR },
	{ "text-decoration-thickness", CSS_PROP_TEXT_DECORATION_THICKNESS },
	{ "list-style-type", CSS_PROP_LIST_STYLE_TYPE },
	{ "top", CSS_PROP_TOP },
	{ "right", CSS_PROP_RIGHT },
	{ "bottom", CSS_PROP_BOTTOM },
	{ "left", CSS_PROP_LEFT },
	{ "z-index", CSS_PROP_Z_INDEX },
	{ "anchor-name", CSS_PROP_ANCHOR_NAME },
	{ "position-anchor", CSS_PROP_POSITION_ANCHOR },
	{ "anchor-scope", CSS_PROP_ANCHOR_SCOPE },
	{ "position-area", CSS_PROP_POSITION_AREA },
	{ "clear", CSS_PROP_CLEAR },
	{ "overflow", SHORT_OVERFLOW },
	{ "transform", SHORT_TRANSFORM },
	{ "-webkit-transform", SHORT_TRANSFORM },
	{ "aspect-ratio", SHORT_ASPECT_RATIO },
	{ "object-fit", CSS_PROP_OBJECT_FIT },
	{ "object-position", SHORT_OBJECT_POSITION },
	{ "transform-origin", SHORT_TRANSFORM_ORIGIN },
	{ "-webkit-transform-origin", SHORT_TRANSFORM_ORIGIN },
	{ "overflow-x", CSS_PROP_OVERFLOW_X },
	{ "overflow-y", CSS_PROP_OVERFLOW_Y },
	{ "overflow-inline", CSS_PROP_OVERFLOW_INLINE },
	{ "overflow-block", CSS_PROP_OVERFLOW_BLOCK },
	{ "box-sizing", CSS_PROP_BOX_SIZING },
	{ "-webkit-box-sizing", CSS_PROP_BOX_SIZING },
	{ "content", CSS_PROP_CONTENT },
	{ "flex-direction", CSS_PROP_FLEX_DIRECTION },
	{ "-webkit-flex-direction", CSS_PROP_FLEX_DIRECTION },
	{ "flex-wrap", CSS_PROP_FLEX_WRAP },
	{ "-webkit-flex-wrap", CSS_PROP_FLEX_WRAP },
	{ "justify-content", CSS_PROP_JUSTIFY_CONTENT },
	{ "-webkit-justify-content", CSS_PROP_JUSTIFY_CONTENT },
	{ "justify-items", CSS_PROP_JUSTIFY_ITEMS },
	{ "align-items", CSS_PROP_ALIGN_ITEMS },
	{ "-webkit-align-items", CSS_PROP_ALIGN_ITEMS },
	{ "align-content", CSS_PROP_ALIGN_CONTENT },
	{ "align-self", CSS_PROP_ALIGN_SELF },
	{ "-webkit-align-self", CSS_PROP_ALIGN_SELF },
	{ "justify-self", CSS_PROP_JUSTIFY_SELF },
	{ "row-gap", CSS_PROP_ROW_GAP },
	{ "grid-row-gap", CSS_PROP_ROW_GAP },
	{ "column-gap", CSS_PROP_COLUMN_GAP },
	{ "grid-column-gap", CSS_PROP_COLUMN_GAP },
	{ "flex-grow", CSS_PROP_FLEX_GROW },
	{ "-webkit-flex-grow", CSS_PROP_FLEX_GROW },
	{ "flex-shrink", CSS_PROP_FLEX_SHRINK },
	{ "-webkit-flex-shrink", CSS_PROP_FLEX_SHRINK },
	{ "flex-basis", CSS_PROP_FLEX_BASIS },
	{ "-webkit-flex-basis", CSS_PROP_FLEX_BASIS },
	{ "order", CSS_PROP_ORDER },
	{ "vertical-align", CSS_PROP_VERTICAL_ALIGN },
	{ "opacity", CSS_PROP_OPACITY },
	{ "box-shadow", CSS_PROP_BOX_SHADOW },
	{ "text-shadow", CSS_PROP_TEXT_SHADOW },
	{ "-webkit-box-shadow", CSS_PROP_BOX_SHADOW },
	{ "-moz-box-shadow", CSS_PROP_BOX_SHADOW },
	{ "outline-width", CSS_PROP_OUTLINE_WIDTH },
	{ "outline-style", CSS_PROP_OUTLINE_STYLE },
	{ "outline-color", CSS_PROP_OUTLINE_COLOR },
	{ "outline-offset", CSS_PROP_OUTLINE_OFFSET },
	{ "clip-path", CSS_PROP_CLIP_PATH },
	{ "clip", CSS_PROP_CLIP },
	{ "direction", CSS_PROP_DIRECTION },
	{ "writing-mode", CSS_PROP_WRITING_MODE },
	{ "text-orientation", CSS_PROP_TEXT_ORIENTATION },
	{ "grid-template-columns", CSS_PROP_GRID_TEMPLATE_COLUMNS },
	{ "grid-template-rows", CSS_PROP_GRID_TEMPLATE_ROWS },
	{ "grid-template-areas", CSS_PROP_GRID_TEMPLATE_AREAS },
	{ "grid-auto-flow", CSS_PROP_GRID_AUTO_FLOW },
	{ "grid-auto-columns", CSS_PROP_GRID_AUTO_COLUMNS },
	{ "grid-auto-rows", CSS_PROP_GRID_AUTO_ROWS },
	{ "grid-column-start", CSS_PROP_GRID_COLUMN_START },
	{ "grid-column-end", CSS_PROP_GRID_COLUMN_END },
	{ "grid-row-start", CSS_PROP_GRID_ROW_START },
	{ "grid-row-end", CSS_PROP_GRID_ROW_END },
	{ "container-type", CSS_PROP_CONTAINER_TYPE },
	{ "border-collapse", CSS_PROP_BORDER_COLLAPSE },
	{ "-webkit-clip-path", CSS_PROP_CLIP_PATH },
	{ "flex", SHORT_FLEX },
	{ "-webkit-flex", SHORT_FLEX },
	{ "flex-flow", SHORT_FLEX_FLOW },
	{ "gap", SHORT_GAP },
	{ "grid-gap", SHORT_GAP },
	{ "border-radius", SHORT_BORDER_RADIUS },
	{ "-webkit-border-radius", SHORT_BORDER_RADIUS },
	{ "-moz-border-radius", SHORT_BORDER_RADIUS },
	{ "border-top-left-radius", SHORT_RADIUS_TOP_LEFT },
	{ "border-top-right-radius", SHORT_RADIUS_TOP_RIGHT },
	{ "border-bottom-right-radius", SHORT_RADIUS_BOTTOM_RIGHT },
	{ "border-bottom-left-radius", SHORT_RADIUS_BOTTOM_LEFT },
	{ "-webkit-border-top-left-radius", SHORT_RADIUS_TOP_LEFT },
	{ "-webkit-border-top-right-radius", SHORT_RADIUS_TOP_RIGHT },
	{ "-webkit-border-bottom-right-radius", SHORT_RADIUS_BOTTOM_RIGHT },
	{ "-webkit-border-bottom-left-radius", SHORT_RADIUS_BOTTOM_LEFT },
	{ "border-start-start-radius", SHORT_RADIUS_TOP_LEFT },
	{ "border-start-end-radius", SHORT_RADIUS_TOP_RIGHT },
	{ "border-end-end-radius", SHORT_RADIUS_BOTTOM_RIGHT },
	{ "border-end-start-radius", SHORT_RADIUS_BOTTOM_LEFT },
	{ "outline", SHORT_OUTLINE },
	{ "grid-column", SHORT_GRID_COLUMN },
	{ "grid-row", SHORT_GRID_ROW },
	{ "grid-area", SHORT_GRID_AREA },
	{ "grid-template", SHORT_GRID_TEMPLATE },
	{ "grid", SHORT_GRID },
	{ "place-items", SHORT_PLACE_ITEMS },
	{ "place-self", SHORT_PLACE_SELF },
	{ "place-content", SHORT_PLACE_CONTENT },
	{ "position-try", SHORT_POSITION_TRY },
	{ "position-try-fallbacks", CSS_PROP_POSITION_TRY_FALLBACKS },
	{ "position-try-order", CSS_PROP_POSITION_TRY_ORDER },
	{ "position-visibility", CSS_PROP_POSITION_VISIBILITY },
	{ "container", SHORT_CONTAINER },
	{ "border-spacing", SHORT_BORDER_SPACING },
	{ "margin", SHORT_MARGIN },
	{ "padding", SHORT_PADDING },
	{ "border", SHORT_BORDER },
	{ "border-top", SHORT_BORDER_TOP },
	{ "border-right", SHORT_BORDER_RIGHT },
	{ "border-bottom", SHORT_BORDER_BOTTOM },
	{ "border-left", SHORT_BORDER_LEFT },
	{ "border-width", SHORT_BORDER_WIDTH },
	{ "border-style", SHORT_BORDER_STYLE },
	{ "border-color", SHORT_BORDER_COLOR },
	{ "background", SHORT_BACKGROUND },
	{ "background-position", SHORT_BACKGROUND_POSITION },
	{ "background-size", SHORT_BACKGROUND_SIZE },
	{ "font", SHORT_FONT },
	{ "text-decoration", SHORT_TEXT_DECORATION },
	{ "list-style", SHORT_LIST_STYLE },

	/* Logical margins retain their identity until the computed flow is known. */
	{ "margin-inline-start", CSS_PROP_MARGIN_INLINE_START },
	{ "margin-inline-end", CSS_PROP_MARGIN_INLINE_END },
	{ "margin-block-start", CSS_PROP_MARGIN_BLOCK_START },
	{ "margin-block-end", CSS_PROP_MARGIN_BLOCK_END },
	{ "padding-inline-start", CSS_PROP_PADDING_INLINE_START },
	{ "padding-inline-end", CSS_PROP_PADDING_INLINE_END },
	{ "padding-block-start", CSS_PROP_PADDING_BLOCK_START },
	{ "padding-block-end", CSS_PROP_PADDING_BLOCK_END },
	{ "inset-inline-start", CSS_PROP_INSET_INLINE_START },
	{ "inset-inline-end", CSS_PROP_INSET_INLINE_END },
	{ "inset-block-start", CSS_PROP_INSET_BLOCK_START },
	{ "inset-block-end", CSS_PROP_INSET_BLOCK_END },
	{ "inline-size", CSS_PROP_INLINE_SIZE },
	{ "block-size", CSS_PROP_BLOCK_SIZE },
	{ "min-inline-size", CSS_PROP_MIN_INLINE_SIZE },
	{ "max-inline-size", CSS_PROP_MAX_INLINE_SIZE },
	{ "min-block-size", CSS_PROP_MIN_BLOCK_SIZE },
	{ "max-block-size", CSS_PROP_MAX_BLOCK_SIZE },
	{ "border-inline-start-width", CSS_PROP_BORDER_INLINE_START_WIDTH },
	{ "border-inline-end-width", CSS_PROP_BORDER_INLINE_END_WIDTH },
	{ "border-block-start-width", CSS_PROP_BORDER_BLOCK_START_WIDTH },
	{ "border-block-end-width", CSS_PROP_BORDER_BLOCK_END_WIDTH },
	{ "border-inline-start-style", CSS_PROP_BORDER_INLINE_START_STYLE },
	{ "border-inline-end-style", CSS_PROP_BORDER_INLINE_END_STYLE },
	{ "border-block-start-style", CSS_PROP_BORDER_BLOCK_START_STYLE },
	{ "border-block-end-style", CSS_PROP_BORDER_BLOCK_END_STYLE },
	{ "border-inline-start-color", CSS_PROP_BORDER_INLINE_START_COLOR },
	{ "border-inline-end-color", CSS_PROP_BORDER_INLINE_END_COLOR },
	{ "border-block-start-color", CSS_PROP_BORDER_BLOCK_START_COLOR },
	{ "border-block-end-color", CSS_PROP_BORDER_BLOCK_END_COLOR },
	{ "border-inline-start", SHORT_BORDER_INLINE_START },
	{ "border-inline-end", SHORT_BORDER_INLINE_END },
	{ "border-block-start", SHORT_BORDER_BLOCK_START },
	{ "border-block-end", SHORT_BORDER_BLOCK_END },
	{ "margin-inline", SHORT_MARGIN_INLINE },
	{ "margin-block", SHORT_MARGIN_BLOCK },
	{ "padding-inline", SHORT_PADDING_INLINE },
	{ "padding-block", SHORT_PADDING_BLOCK },
	{ "inset", SHORT_INSET },
	{ "inset-inline", SHORT_INSET_INLINE },
	{ "inset-block", SHORT_INSET_BLOCK },
	{ NULL, 0 }
};

/* The keywords of display. */
static const struct values_keyword values_display[] = {
	{ "inline", CSS_DISPLAY_INLINE },
	{ "block", CSS_DISPLAY_BLOCK },
	{ "inline-block", CSS_DISPLAY_INLINE_BLOCK },
	{ "list-item", CSS_DISPLAY_LIST_ITEM },
	{ "none", CSS_DISPLAY_NONE },
	{ "table", CSS_DISPLAY_TABLE },
	{ "inline-table", CSS_DISPLAY_INLINE_TABLE },
	{ "table-row", CSS_DISPLAY_TABLE_ROW },
	{ "table-cell", CSS_DISPLAY_TABLE_CELL },
	{ "table-row-group", CSS_DISPLAY_TABLE_ROW_GROUP },
	{ "table-header-group", CSS_DISPLAY_TABLE_ROW_GROUP },
	{ "table-footer-group", CSS_DISPLAY_TABLE_ROW_GROUP },
	{ "table-caption", CSS_DISPLAY_TABLE_CAPTION },
	{ "table-column", CSS_DISPLAY_TABLE_COLUMN },
	{ "table-column-group", CSS_DISPLAY_TABLE_COLUMN },
	{ "flex", CSS_DISPLAY_FLEX },
	{ "inline-flex", CSS_DISPLAY_INLINE_FLEX },
	{ "-webkit-flex", CSS_DISPLAY_FLEX },
	{ "-webkit-inline-flex", CSS_DISPLAY_INLINE_FLEX },
	{ "-webkit-box", CSS_DISPLAY_BLOCK },
	{ "-webkit-inline-box", CSS_DISPLAY_INLINE_BLOCK },
	{ "grid", CSS_DISPLAY_GRID },
	{ "inline-grid", CSS_DISPLAY_INLINE_GRID },
	{ "flow-root", CSS_DISPLAY_BLOCK },
	{ "contents", CSS_DISPLAY_CONTENTS },
	{ NULL, 0 }
};

/* The keywords of border-collapse. */
static const struct values_keyword values_border_collapse[] = {
	{ "separate", 0 },
	{ "collapse", 1 },
	{ NULL, 0 }
};

/* The keywords of container-type. */
static const struct values_keyword values_container_type[] = {
	{ "normal", CSS_CONTAINER_NORMAL },
	{ "inline-size", CSS_CONTAINER_INLINE_SIZE },
	{ "size", CSS_CONTAINER_SIZE },
	{ NULL, 0 }
};

/* The keywords of direction. */
static const struct values_keyword values_direction[] = {
	{ "ltr", CSS_DIRECTION_LTR },
	{ "rtl", CSS_DIRECTION_RTL },
	{ NULL, 0 }
};

/* Vertical-lr is laid out in Flexbox; vertical-rl is retained for Grid item classification. */
static const struct values_keyword values_writing_mode[] = {
	{ "horizontal-tb", CSS_WRITING_HORIZONTAL_TB },
	{ "vertical-lr", CSS_WRITING_VERTICAL_LR },
	{ "vertical-rl", CSS_WRITING_VERTICAL_RL },
	{ "sideways-lr", 3 },
	{ "sideways-rl", 4 },
	{ NULL, 0 }
};

/* The glyph orientation retained for vertical inline layout. */
static const struct values_keyword values_text_orientation[] = {
	{ "mixed", CSS_TEXT_ORIENTATION_MIXED },
	{ "upright", CSS_TEXT_ORIENTATION_UPRIGHT },
	{ "sideways", CSS_TEXT_ORIENTATION_SIDEWAYS },
	{ NULL, 0 }
};

/* The keywords of vertical-align (a length or percentage is the other form). */
static const struct values_keyword values_vertical_align[] = {
	{ "baseline", CSS_VALIGN_BASELINE },
	{ "top", CSS_VALIGN_TOP },
	{ "middle", CSS_VALIGN_MIDDLE },
	{ "bottom", CSS_VALIGN_BOTTOM },
	{ "text-top", CSS_VALIGN_TEXT_TOP },
	{ "text-bottom", CSS_VALIGN_TEXT_BOTTOM },
	{ "sub", CSS_VALIGN_SUB },
	{ "super", CSS_VALIGN_SUPER },
	{ NULL, 0 }
};

/* The keywords of flex-direction. */
static const struct values_keyword values_flex_direction[] = {
	{ "row", CSS_FLEX_ROW },
	{ "row-reverse", CSS_FLEX_ROW_REVERSE },
	{ "column", CSS_FLEX_COLUMN },
	{ "column-reverse", CSS_FLEX_COLUMN_REVERSE },
	{ NULL, 0 }
};

/* The keywords of flex-wrap. */
static const struct values_keyword values_flex_wrap[] = {
	{ "nowrap", CSS_FLEX_NOWRAP },
	{ "wrap", CSS_FLEX_WRAP },
	{ "wrap-reverse", CSS_FLEX_WRAP_REVERSE },
	{ NULL, 0 }
};

/* The keywords of the alignment properties; justify-* keeps physical left and right distinct. */
static const struct values_keyword values_align[] = {
	{ "auto", CSS_ALIGN_AUTO },
	{ "normal", CSS_ALIGN_NORMAL },
	{ "stretch", CSS_ALIGN_STRETCH },
	{ "flex-start", CSS_ALIGN_FLEX_START },
	{ "start", CSS_ALIGN_START },
	{ "self-start", CSS_ALIGN_SELF_START },
	{ "left", CSS_ALIGN_LEFT },
	{ "flex-end", CSS_ALIGN_FLEX_END },
	{ "end", CSS_ALIGN_END },
	{ "self-end", CSS_ALIGN_SELF_END },
	{ "right", CSS_ALIGN_RIGHT },
	{ "center", CSS_ALIGN_CENTER },
	{ "baseline", CSS_ALIGN_BASELINE },
	{ "first", CSS_ALIGN_BASELINE },
	{ "space-between", CSS_ALIGN_SPACE_BETWEEN },
	{ "space-around", CSS_ALIGN_SPACE_AROUND },
	{ "space-evenly", CSS_ALIGN_SPACE_EVENLY },
	{ "anchor-center", CSS_ALIGN_ANCHOR_CENTER },
	{ NULL, 0 }
};

/* The keywords of position. */
static const struct values_keyword values_position[] = {
	{ "static", CSS_POSITION_STATIC },
	{ "relative", CSS_POSITION_RELATIVE },
	{ "absolute", CSS_POSITION_ABSOLUTE },
	{ "fixed", CSS_POSITION_FIXED },
	{ "sticky", CSS_POSITION_STICKY },
	{ NULL, 0 }
};

/* The keywords of float. */
static const struct values_keyword values_float[] = {
	{ "none", CSS_FLOAT_NONE },
	{ "left", CSS_FLOAT_LEFT },
	{ "right", CSS_FLOAT_RIGHT },
	{ "inline-start", CSS_FLOAT_LEFT },
	{ "inline-end", CSS_FLOAT_RIGHT },
	{ NULL, 0 }
};

/* The keywords of overflow (one value for both axes in this pass). */
static const struct values_keyword values_overflow[] = {
	{ "visible", CSS_OVERFLOW_VISIBLE },
	{ "hidden", CSS_OVERFLOW_HIDDEN },
	{ "clip", CSS_OVERFLOW_CLIP },
	{ "scroll", CSS_OVERFLOW_SCROLL },
	{ "auto", CSS_OVERFLOW_AUTO },
	{ "overlay", CSS_OVERFLOW_AUTO },
	{ NULL, 0 }
};

/* The keywords of clear. */
static const struct values_keyword values_clear[] = {
	{ "none", CSS_FLOAT_NONE },
	{ "left", CSS_FLOAT_LEFT },
	{ "right", CSS_FLOAT_RIGHT },
	{ "both", CSS_CLEAR_BOTH },
	{ "inline-start", CSS_FLOAT_LEFT },
	{ "inline-end", CSS_FLOAT_RIGHT },
	{ NULL, 0 }
};

/* The keywords of visibility. */
static const struct values_keyword values_visibility[] = {
	{ "visible", 0 },
	{ "hidden", 1 },
	{ "collapse", 1 },
	{ NULL, 0 }
};

/* The keywords of border-style. */
static const struct values_keyword values_border_style[] = {
	{ "none", CSS_BORDER_NONE },
	{ "hidden", CSS_BORDER_NONE },
	{ "solid", CSS_BORDER_SOLID },
	{ "dashed", CSS_BORDER_DASHED },
	{ "dotted", CSS_BORDER_DOTTED },
	{ "double", CSS_BORDER_DOUBLE },
	{ "groove", CSS_BORDER_SOLID },
	{ "ridge", CSS_BORDER_SOLID },
	{ "inset", CSS_BORDER_INSET },
	{ "outset", CSS_BORDER_OUTSET },
	{ NULL, 0 }
};

/* The keywords of box-sizing. */
static const struct values_keyword values_box_sizing[] = {
	{ "content-box", CSS_BOX_SIZING_CONTENT },
	{ "border-box", CSS_BOX_SIZING_BORDER },
	{ NULL, 0 }
};

/* The keywords of border widths, in pixels. */
static const struct values_keyword values_border_width[] = {
	{ "thin", 1 },
	{ "medium", 3 },
	{ "thick", 5 },
	{ NULL, 0 }
};

/* The keywords of font-weight. */
static const struct values_keyword values_font_weight[] = {
	{ "normal", 400 },
	{ "bold", 700 },
	{ "bolder", 700 },
	{ "lighter", 300 },
	{ NULL, 0 }
};

/* The keywords of font-style. */
static const struct values_keyword values_font_style[] = {
	{ "normal", 0 },
	{ "italic", 1 },
	{ "oblique", 1 },
	{ NULL, 0 }
};

/* The keywords of font-size, in pixels at the default size of 16. */
static const struct values_keyword values_font_size[] = {
	{ "xx-small", 9 },
	{ "x-small", 10 },
	{ "small", 13 },
	{ "medium", 16 },
	{ "large", 18 },
	{ "x-large", 24 },
	{ "xx-large", 32 },
	{ "xxx-large", 48 },
	{ NULL, 0 }
};

/* The keywords of text-align. */
static const struct values_keyword values_text_align[] = {
	{ "start", CSS_TEXT_ALIGN_START },
	{ "left", CSS_TEXT_ALIGN_LEFT },
	{ "right", CSS_TEXT_ALIGN_RIGHT },
	{ "center", CSS_TEXT_ALIGN_CENTER },
	{ "justify", CSS_TEXT_ALIGN_JUSTIFY },
	{ "end", CSS_TEXT_ALIGN_END },
	{ "-webkit-center", CSS_TEXT_ALIGN_CENTER },
	{ NULL, 0 }
};

/* The supported predefined cursor keywords, shared by ordinary and inline declarations. */
static const struct values_keyword values_cursor[] = {
	{ "auto", CSS_CURSOR_AUTO },
	{ "default", CSS_CURSOR_DEFAULT },
	{ "none", CSS_CURSOR_NONE },
	{ "context-menu", CSS_CURSOR_CONTEXT_MENU },
	{ "help", CSS_CURSOR_HELP },
	{ "pointer", CSS_CURSOR_POINTER },
	{ "progress", CSS_CURSOR_PROGRESS },
	{ "wait", CSS_CURSOR_WAIT },
	{ "cell", CSS_CURSOR_CELL },
	{ "crosshair", CSS_CURSOR_CROSSHAIR },
	{ "text", CSS_CURSOR_TEXT },
	{ "vertical-text", CSS_CURSOR_VERTICAL_TEXT },
	{ "alias", CSS_CURSOR_ALIAS },
	{ "copy", CSS_CURSOR_COPY },
	{ "move", CSS_CURSOR_MOVE },
	{ "no-drop", CSS_CURSOR_NO_DROP },
	{ "not-allowed", CSS_CURSOR_NOT_ALLOWED },
	{ "e-resize", CSS_CURSOR_E_RESIZE },
	{ "n-resize", CSS_CURSOR_N_RESIZE },
	{ "ne-resize", CSS_CURSOR_NE_RESIZE },
	{ "nw-resize", CSS_CURSOR_NW_RESIZE },
	{ "s-resize", CSS_CURSOR_S_RESIZE },
	{ "se-resize", CSS_CURSOR_SE_RESIZE },
	{ "sw-resize", CSS_CURSOR_SW_RESIZE },
	{ "w-resize", CSS_CURSOR_W_RESIZE },
	{ "ew-resize", CSS_CURSOR_EW_RESIZE },
	{ "ns-resize", CSS_CURSOR_NS_RESIZE },
	{ "nesw-resize", CSS_CURSOR_NESW_RESIZE },
	{ "nwse-resize", CSS_CURSOR_NWSE_RESIZE },
	{ "col-resize", CSS_CURSOR_COL_RESIZE },
	{ "row-resize", CSS_CURSOR_ROW_RESIZE },
	{ "all-scroll", CSS_CURSOR_ALL_SCROLL },
	{ "grab", CSS_CURSOR_GRAB },
	{ "grabbing", CSS_CURSOR_GRABBING },
	{ "zoom-in", CSS_CURSOR_ZOOM_IN },
	{ "zoom-out", CSS_CURSOR_ZOOM_OUT },
	{ NULL, 0 }
};

/* The native computed text-transform keywords accepted by the CSS2 property. */
static const struct values_keyword values_text_transform[] = {
	{ "none", CSS_TEXT_TRANSFORM_NONE },
	{ "capitalize", CSS_TEXT_TRANSFORM_CAPITALIZE },
	{ "uppercase", CSS_TEXT_TRANSFORM_UPPERCASE },
	{ "lowercase", CSS_TEXT_TRANSFORM_LOWERCASE },
	{ NULL, 0 }
};

/* The keywords of object-fit. */
static const struct values_keyword values_object_fit[] = {
	{ "fill", CSS_OBJECT_FIT_FILL },
	{ "contain", CSS_OBJECT_FIT_CONTAIN },
	{ "cover", CSS_OBJECT_FIT_COVER },
	{ "none", CSS_OBJECT_FIT_NONE },
	{ "scale-down", CSS_OBJECT_FIT_SCALE_DOWN },
	{ NULL, 0 }
};

/* The keywords of white-space. */
static const struct values_keyword values_white_space[] = {
	{ "normal", CSS_WHITE_SPACE_NORMAL },
	{ "pre", CSS_WHITE_SPACE_PRE },
	{ "nowrap", CSS_WHITE_SPACE_NOWRAP },
	{ "pre-wrap", CSS_WHITE_SPACE_PRE_WRAP },
	{ "pre-line", CSS_WHITE_SPACE_PRE_LINE },
	{ NULL, 0 }
};

/* The keywords of text-decoration-style. */
static const struct values_keyword values_decoration_styles[] = {
	{ "solid", CSS_DECORATION_SOLID },
	{ "double", CSS_DECORATION_DOUBLE },
	{ "dotted", CSS_DECORATION_DOTTED },
	{ "dashed", CSS_DECORATION_DASHED },
	{ "wavy", CSS_DECORATION_WAVY },
	{ NULL, 0 }
};

/* The keywords of text-decoration-skip-ink. */
static const struct values_keyword values_decoration_skip_inks[] = {
	{ "auto", CSS_DECORATION_SKIP_AUTO },
	{ "none", CSS_DECORATION_SKIP_NONE },
	{ "all", CSS_DECORATION_SKIP_ALL },
	{ NULL, 0 }
};

/* The keywords of list-style-type. */
static const struct values_keyword values_list_style[] = {
	{ "disc", CSS_LIST_DISC },
	{ "circle", CSS_LIST_CIRCLE },
	{ "square", CSS_LIST_SQUARE },
	{ "decimal", CSS_LIST_DECIMAL },
	{ "none", CSS_LIST_NONE },
	{ NULL, 0 }
};

/* The CSS-wide keywords. */
static const struct values_keyword values_wide[] = {
	{ "inherit", CSS_VALUE_INHERIT },
	{ "initial", CSS_VALUE_INITIAL },
	{ "unset", CSS_VALUE_UNSET },
	{ "revert", CSS_VALUE_UNSET },
	{ "revert-layer", CSS_VALUE_UNSET },
	{ NULL, 0 }
};

/* The values of background-repeat's keywords (space and round are drawn as repeat). */
static const struct values_keyword values_repeat[] = {
	{ "repeat", CSS_REPEAT_BOTH },
	{ "repeat-x", CSS_REPEAT_X },
	{ "repeat-y", CSS_REPEAT_Y },
	{ "no-repeat", CSS_REPEAT_NONE },
	{ "space", CSS_REPEAT_BOTH },
	{ "round", CSS_REPEAT_BOTH },
	{ NULL, 0 }
};

/* The values of background-attachment (local currently scrolls with its box). */
static const struct values_keyword values_attachment[] = {
	{ "scroll", CSS_BACKGROUND_SCROLL },
	{ "fixed", CSS_BACKGROUND_FIXED },
	{ "local", CSS_BACKGROUND_SCROLL },
	{ NULL, 0 }
};

/* The keywords of background-position, as percentages of the room. */
static const struct values_keyword values_position_keywords[] = {
	{ "left", 0 },
	{ "center", 50 },
	{ "right", 100 },
	{ "top", 0 },
	{ "bottom", 100 },
	{ NULL, 0 }
};

/* The keywords a length property may take instead of a length. */
static const struct values_keyword values_length_keywords[] = {
	{ "auto", CSS_UNIT_AUTO },
	{ "none", CSS_UNIT_NONE },
	{ "normal", CSS_UNIT_NORMAL },
	{ "max-content", CSS_UNIT_MAX_CONTENT },
	{ "-webkit-max-content", CSS_UNIT_MAX_CONTENT },
	{ "-moz-max-content", CSS_UNIT_MAX_CONTENT },
	{ "min-content", CSS_UNIT_MIN_CONTENT },
	{ "-webkit-min-content", CSS_UNIT_MIN_CONTENT },
	{ "-moz-min-content", CSS_UNIT_MIN_CONTENT },
	{ "fit-content", CSS_UNIT_FIT_CONTENT },
	{ "-webkit-fit-content", CSS_UNIT_FIT_CONTENT },
	{ "-moz-fit-content", CSS_UNIT_FIT_CONTENT },
	{ NULL, 0 }
};

/* The units of lengths. */
static const struct values_keyword values_units[] = {
	{ "px", CSS_DUNIT_PX },
	{ "em", CSS_DUNIT_EM },
	{ "rem", CSS_DUNIT_REM },
	{ "ex", CSS_DUNIT_EX },
	{ "ch", CSS_DUNIT_EX },
	{ "pt", CSS_DUNIT_PT },
	{ "pc", CSS_DUNIT_PC },
	{ "in", CSS_DUNIT_IN },
	{ "cm", CSS_DUNIT_CM },
	{ "mm", CSS_DUNIT_MM },
	{ "vw", CSS_DUNIT_VW },
	{ "vh", CSS_DUNIT_VH },
	{ "vmin", CSS_DUNIT_VMIN },
	{ "vmax", CSS_DUNIT_VMAX },
	{ "svw", CSS_DUNIT_VW },
	{ "lvw", CSS_DUNIT_VW },
	{ "dvw", CSS_DUNIT_VW },
	{ "svh", CSS_DUNIT_VH },
	{ "lvh", CSS_DUNIT_VH },
	{ "dvh", CSS_DUNIT_VH },
	{ "cqw", CSS_DUNIT_CQW },
	{ "cqi", CSS_DUNIT_CQW },
	{ "cqmin", CSS_DUNIT_CQW },
	{ "cqmax", CSS_DUNIT_CQW },
	{ "cqh", CSS_DUNIT_CQH },
	{ "cqb", CSS_DUNIT_CQH },
	{ NULL, 0 }
};

/* The relative font sizes, as percentages of the parent's. */
static const struct values_keyword values_relative_size[] = {
	{ "smaller", 83 },
	{ "larger", 120 },
	{ NULL, 0 }
};

/* The words of the font shorthand this pass reads past. */
static const struct values_keyword values_font_ignored[] = {
	{ "normal", 0 },
	{ "small-caps", 0 },
	{ NULL, 0 }
};

/* The color keywords that are not named colors (CSS_CURRENT_COLOR stands for currentcolor). */
static const struct values_keyword values_color_keywords[] = {
	{ "transparent", 0 },
	{ "currentcolor", (int)CSS_CURRENT_COLOR },
	{ NULL, 0 }
};

/* The named colors of CSS Color 4. */
static const struct values_color values_colors[] = {
	{ "aliceblue", 0xf0f8ffU },
	{ "antiquewhite", 0xfaebd7U },
	{ "aqua", 0x00ffffU },
	{ "aquamarine", 0x7fffd4U },
	{ "azure", 0xf0ffffU },
	{ "beige", 0xf5f5dcU },
	{ "bisque", 0xffe4c4U },
	{ "black", 0x000000U },
	{ "blanchedalmond", 0xffebcdU },
	{ "blue", 0x0000ffU },
	{ "blueviolet", 0x8a2be2U },
	{ "brown", 0xa52a2aU },
	{ "burlywood", 0xdeb887U },
	{ "cadetblue", 0x5f9ea0U },
	{ "chartreuse", 0x7fff00U },
	{ "chocolate", 0xd2691eU },
	{ "coral", 0xff7f50U },
	{ "cornflowerblue", 0x6495edU },
	{ "cornsilk", 0xfff8dcU },
	{ "crimson", 0xdc143cU },
	{ "cyan", 0x00ffffU },
	{ "darkblue", 0x00008bU },
	{ "darkcyan", 0x008b8bU },
	{ "darkgoldenrod", 0xb8860bU },
	{ "darkgray", 0xa9a9a9U },
	{ "darkgreen", 0x006400U },
	{ "darkgrey", 0xa9a9a9U },
	{ "darkkhaki", 0xbdb76bU },
	{ "darkmagenta", 0x8b008bU },
	{ "darkolivegreen", 0x556b2fU },
	{ "darkorange", 0xff8c00U },
	{ "darkorchid", 0x9932ccU },
	{ "darkred", 0x8b0000U },
	{ "darksalmon", 0xe9967aU },
	{ "darkseagreen", 0x8fbc8fU },
	{ "darkslateblue", 0x483d8bU },
	{ "darkslategray", 0x2f4f4fU },
	{ "darkslategrey", 0x2f4f4fU },
	{ "darkturquoise", 0x00ced1U },
	{ "darkviolet", 0x9400d3U },
	{ "deeppink", 0xff1493U },
	{ "deepskyblue", 0x00bfffU },
	{ "dimgray", 0x696969U },
	{ "dimgrey", 0x696969U },
	{ "dodgerblue", 0x1e90ffU },
	{ "firebrick", 0xb22222U },
	{ "floralwhite", 0xfffaf0U },
	{ "forestgreen", 0x228b22U },
	{ "fuchsia", 0xff00ffU },
	{ "gainsboro", 0xdcdcdcU },
	{ "ghostwhite", 0xf8f8ffU },
	{ "gold", 0xffd700U },
	{ "goldenrod", 0xdaa520U },
	{ "gray", 0x808080U },
	{ "green", 0x008000U },
	{ "greenyellow", 0xadff2fU },
	{ "grey", 0x808080U },
	{ "honeydew", 0xf0fff0U },
	{ "hotpink", 0xff69b4U },
	{ "indianred", 0xcd5c5cU },
	{ "indigo", 0x4b0082U },
	{ "ivory", 0xfffff0U },
	{ "khaki", 0xf0e68cU },
	{ "lavender", 0xe6e6faU },
	{ "lavenderblush", 0xfff0f5U },
	{ "lawngreen", 0x7cfc00U },
	{ "lemonchiffon", 0xfffacdU },
	{ "lightblue", 0xadd8e6U },
	{ "lightcoral", 0xf08080U },
	{ "lightcyan", 0xe0ffffU },
	{ "lightgoldenrodyellow", 0xfafad2U },
	{ "lightgray", 0xd3d3d3U },
	{ "lightgreen", 0x90ee90U },
	{ "lightgrey", 0xd3d3d3U },
	{ "lightpink", 0xffb6c1U },
	{ "lightsalmon", 0xffa07aU },
	{ "lightseagreen", 0x20b2aaU },
	{ "lightskyblue", 0x87cefaU },
	{ "lightslategray", 0x778899U },
	{ "lightslategrey", 0x778899U },
	{ "lightsteelblue", 0xb0c4deU },
	{ "lightyellow", 0xffffe0U },
	{ "lime", 0x00ff00U },
	{ "limegreen", 0x32cd32U },
	{ "linen", 0xfaf0e6U },
	{ "magenta", 0xff00ffU },
	{ "maroon", 0x800000U },
	{ "mediumaquamarine", 0x66cdaaU },
	{ "mediumblue", 0x0000cdU },
	{ "mediumorchid", 0xba55d3U },
	{ "mediumpurple", 0x9370dbU },
	{ "mediumseagreen", 0x3cb371U },
	{ "mediumslateblue", 0x7b68eeU },
	{ "mediumspringgreen", 0x00fa9aU },
	{ "mediumturquoise", 0x48d1ccU },
	{ "mediumvioletred", 0xc71585U },
	{ "midnightblue", 0x191970U },
	{ "mintcream", 0xf5fffaU },
	{ "mistyrose", 0xffe4e1U },
	{ "moccasin", 0xffe4b5U },
	{ "navajowhite", 0xffdeadU },
	{ "navy", 0x000080U },
	{ "oldlace", 0xfdf5e6U },
	{ "olive", 0x808000U },
	{ "olivedrab", 0x6b8e23U },
	{ "orange", 0xffa500U },
	{ "orangered", 0xff4500U },
	{ "orchid", 0xda70d6U },
	{ "palegoldenrod", 0xeee8aaU },
	{ "palegreen", 0x98fb98U },
	{ "paleturquoise", 0xafeeeeU },
	{ "palevioletred", 0xdb7093U },
	{ "papayawhip", 0xffefd5U },
	{ "peachpuff", 0xffdab9U },
	{ "peru", 0xcd853fU },
	{ "pink", 0xffc0cbU },
	{ "plum", 0xdda0ddU },
	{ "powderblue", 0xb0e0e6U },
	{ "purple", 0x800080U },
	{ "rebeccapurple", 0x663399U },
	{ "red", 0xff0000U },
	{ "rosybrown", 0xbc8f8fU },
	{ "royalblue", 0x4169e1U },
	{ "saddlebrown", 0x8b4513U },
	{ "salmon", 0xfa8072U },
	{ "sandybrown", 0xf4a460U },
	{ "seagreen", 0x2e8b57U },
	{ "seashell", 0xfff5eeU },
	{ "sienna", 0xa0522dU },
	{ "silver", 0xc0c0c0U },
	{ "skyblue", 0x87ceebU },
	{ "slateblue", 0x6a5acdU },
	{ "slategray", 0x708090U },
	{ "slategrey", 0x708090U },
	{ "snow", 0xfffafaU },
	{ "springgreen", 0x00ff7fU },
	{ "steelblue", 0x4682b4U },
	{ "tan", 0xd2b48cU },
	{ "teal", 0x008080U },
	{ "thistle", 0xd8bfd8U },
	{ "tomato", 0xff6347U },
	{ "turquoise", 0x40e0d0U },
	{ "violet", 0xee82eeU },
	{ "wheat", 0xf5deb3U },
	{ "white", 0xffffffU },
	{ "whitesmoke", 0xf5f5f5U },
	{ "yellow", 0xffff00U },
	{ "yellowgreen", 0x9acd32U },
	{ NULL, 0 }
};

static int values_keyword(const struct values_keyword *table, const struct css_token *token, int *value);
static int values_unit(const struct css_token *token, int *unit);
static int values_length(const struct css_token *token, int allow_keywords, struct css_value *value);
static int values_flex_basis(const struct css_token *token, struct css_value *value);
static int values_single(struct css_parse *parse, int property, const struct css_token *tokens, size_t count, struct css_value *value);
static int values_families(struct css_parse *parse, const struct css_token *tokens, size_t count, struct css_value *value);
static size_t values_components(const struct css_token *tokens, size_t count, size_t *starts, size_t *lengths, size_t max);
static int values_transform(const struct css_token *tokens, size_t count, struct css_declaration *out, size_t *made);
static int values_aspect_ratio(const struct css_token *tokens, size_t count, struct css_declaration *out, size_t *made);
static int values_transform_origin(const struct css_token *tokens, size_t count, int first_property, struct css_declaration *out, size_t *made);
static int values_four(struct css_parse *parse, int first, const struct css_token *tokens, size_t count, struct css_declaration *out, size_t *made);
static int values_border(struct css_parse *parse, int side, const struct css_token *tokens, size_t count, struct css_declaration *out, size_t *made);
static void values_add(struct css_declaration *out, size_t *made, int property, const struct css_value *value);
static int values_hex_digit(uint16_t unit);
static int values_rgb_function(const struct css_token *tokens, size_t count, uint32_t *color);
static int values_hsl_function(const struct css_token *tokens, size_t count, uint32_t *color);
static void values_wide_keyword(int property, const struct css_value *value, struct css_declaration *out, size_t *made);
static int values_background(struct css_parse *parse, const struct css_token *tokens, size_t count, struct css_declaration *out, size_t *made);
static size_t values_function_length(const struct css_token *tokens, size_t count);
static int values_url(struct css_parse *parse, const struct css_token *tokens, size_t count, struct css_value *value);
static int values_repeat_parts(const struct css_token *const *parts, size_t count, struct css_value *value);
static int values_subgrid(struct css_parse *parse, const struct css_token *tokens, size_t count, struct css_track_list *list);
static int values_position_parts(const struct css_token *const *parts, size_t count, struct css_value *x, struct css_value *y);
static int values_position_part(const struct css_token *token, struct css_value *value);
static int values_position_axis(const struct css_token *token);
static int values_size_parts(const struct css_token *const *parts, size_t count, struct css_value *width, struct css_value *height);
static int values_background_position(const struct css_token *tokens, size_t count, struct css_declaration *out, size_t *made);
static int values_background_size(const struct css_token *tokens, size_t count, struct css_declaration *out, size_t *made);
static int values_takes_length(int property);
static int values_flex(struct css_parse *parse, const struct css_token *tokens, size_t count, struct css_declaration *out, size_t *made);
static int values_unitless_function(const struct css_token *tokens, size_t count);
static int values_position_try(struct css_parse *parse, const struct css_token *tokens, size_t count, struct css_declaration *out, size_t *made);
static int values_place(struct css_parse *parse, const struct css_token *tokens, size_t count, int align, int justify, struct css_declaration *out, size_t *made);
static size_t values_place_value(const struct css_token *tokens, const size_t *starts, size_t components, size_t first);
static int values_flex_flow(struct css_parse *parse, const struct css_token *tokens, size_t count, struct css_declaration *out, size_t *made);
static int values_content(struct css_parse *parse, const struct css_token *tokens, size_t count, struct css_value *value);
static int values_pair(struct css_parse *parse, int first, int second, const struct css_token *tokens, size_t count, struct css_declaration *out, size_t *made);
static int values_calc(struct css_parse *parse, int property, const struct css_token *tokens, size_t count, struct css_value *value);
static int values_calc_arguments(struct wb_arena *arena, const struct css_token *tokens, size_t count, int operation, struct css_calc *calc);
static int values_calc_sum(struct wb_arena *arena, const struct css_token *tokens, size_t count, size_t *index, struct values_term *term);
static int values_calc_product(struct wb_arena *arena, const struct css_token *tokens, size_t count, size_t *index, struct values_term *term);
static int values_calc_value(struct wb_arena *arena, const struct css_token *tokens, size_t count, size_t *index, struct values_term *term);
static int values_calc_unit(const struct css_token *token, struct css_calc_sum *sum);
static void values_calc_scale(struct css_calc_sum *sum, float factor);
static int values_calc_add(struct css_calc_sum *sum, const struct css_calc_sum *other, float sign);
static size_t values_skip_space(const struct css_token *tokens, size_t count, size_t index);
static int values_border_radius(struct css_parse *parse, const struct css_token *tokens, size_t count, struct css_declaration *out, size_t *made);
static int values_corner_radius(struct css_parse *parse, int corner, const struct css_token *tokens, size_t count, struct css_declaration *out, size_t *made);
static int values_radii(struct css_parse *parse, const struct css_token *tokens, size_t count, struct css_value *radii);
static int values_box_shadow(struct css_parse *parse, const struct css_token *tokens, size_t count, struct css_value *value);
static int values_text_shadow(struct css_parse *parse, const struct css_token *tokens, size_t count, struct css_value *value);
static int values_one_shadow(const struct css_token *tokens, size_t count, struct css_declared_shadow *shadow);
static int values_outline(struct css_parse *parse, const struct css_token *tokens, size_t count, struct css_declaration *out, size_t *made);
static int values_clip_path(struct css_parse *parse, const struct css_token *tokens, size_t count, struct css_value *value);
static int values_clip(struct css_parse *parse, const struct css_token *tokens, size_t count, struct css_value *value);
static int values_tracks(struct css_parse *parse, const struct css_token *tokens, size_t count, struct css_value *value);
static int values_auto_tracks(struct css_parse *parse, const struct css_token *tokens, size_t count, struct css_value *value);
static int values_grid_auto_flow(const struct css_token *tokens, size_t count, struct css_value *value);
static int values_track_list(struct css_parse *parse, const struct css_token *tokens, size_t count, struct css_track_list *list, int depth);
static void values_line_name(struct css_track_list *list, size_t line, struct vm_string *name);
static int values_areas(struct css_parse *parse, const struct css_token *tokens, size_t count, struct css_value *value);
static int values_area_row(struct css_parse *parse, const struct css_token *token, struct css_grid_areas *areas, int *columns);
static int values_area_name_unit(uint16_t unit);
static int values_areas_check(const struct css_grid_areas *areas);
static int values_area_seen(const struct css_grid_areas *areas, struct vm_string *name, int row, int column);
static void values_copy_line_names(struct css_track_list *list, size_t from, size_t to, int skip, int end);
static int values_track_minimum_rule(const struct css_token *token, int kind);
static int values_track_maximum_rule(const struct css_token *token, int kind);
static int values_one_track(struct css_parse *parse, const struct css_token *tokens, size_t count, struct css_declared_track *track);
static int values_track_size(struct css_parse *parse, const struct css_token *tokens, size_t count, int *kind, struct css_value *size);
static size_t values_split_at(const struct css_token *tokens, size_t count, int type, uint32_t delim);
static int values_physical_grammar(int property);
static int values_grid_line(struct css_parse *parse, const struct css_token *tokens, size_t count, struct css_value *value);
static int values_custom_ident(struct css_parse *parse, const struct css_token *token, struct vm_string **name);
static int values_grid_line_is_name(const struct css_value *line);
static int values_grid_pair(struct css_parse *parse, int start, int end, const struct css_token *tokens, size_t count, struct css_declaration *out, size_t *made);
static int values_grid_area(struct css_parse *parse, const struct css_token *tokens, size_t count, struct css_declaration *out, size_t *made);
static int values_grid_explicit(struct css_parse *parse, const struct css_token *tokens, size_t count, struct css_declaration *out, size_t *made);
static int values_grid_template(struct css_parse *parse, const struct css_token *tokens, size_t count, struct css_declaration *out, size_t *made);
static int values_grid_template_areas(struct css_parse *parse, const struct css_token *tokens, size_t count, struct css_declaration *out, size_t *made);
static int values_tracks_none(struct css_parse *parse, struct css_value *value);
static int values_grid(struct css_parse *parse, const struct css_token *tokens, size_t count, struct css_declaration *out, size_t *made);
static int values_grid_side(struct css_parse *parse, int property, const struct css_token *tokens, size_t count, struct css_declaration *out, size_t *made);
static int values_decoration_line(const struct css_token *tokens, size_t count, struct css_value *value);
static int values_text_decoration(const struct css_token *tokens, size_t count, struct css_declaration *out, size_t *made);


/*
 * Reports the name of the index-th property this pass knows (a longhand
 * or a shorthand), or NULL past the last one: the names the inline style
 * of scripts has members for.
 */
const char *
css_property_name(
	size_t index)
{
	size_t at;

	/* Counts up to the index, stopping at the table's end. */
	for (at = 0; at < index; at++) {
		if (values_names[at].name == NULL)
			return NULL;
	}

	/* Succeeded: the name, or NULL at the end. */
	return values_names[index].name;
}

/*
 * Finds a property's number by its name, or -1 for an unknown property.
 */
int
css_property_lookup(
	const struct css_token *name)
{
	size_t index;
	int same;

	/* Compares with each known name. */
	for (index = 0; values_names[index].name != NULL; index++) {
		same = css_ident_equal(name, values_names[index].name);
		if (same)
			return values_names[index].property;
	}

	/* The property is not one this pass knows. */
	return -1;
}

/*
 * Parses a declaration's value into longhand declarations.
 *
 * Returns EINVAL for an unknown property or an invalid value (the
 * declaration is then dropped) and ENOMEM when memory runs out.
 */
int
css_parse_value(
	struct css_parse *parse,
	const struct css_token *tokens,
	size_t count,
	const struct css_token *name,
	struct css_declaration *out,
	size_t *out_count,
	size_t out_capacity)
{
	struct css_declaration expanded[VALUES_EXPANSION_MAX];
	size_t made;
	int property;
	int error;

	/* Finds the property. */
	*out_count = 0;
	property = css_property_lookup(name);
	if (property < 0)
		return EINVAL;

	/* Parse into bounded scratch before publishing declarations to a caller's smaller buffer. */
	made = 0;
	error = css_parse_property(parse, property, tokens, count, expanded, &made);
	if (error != 0)
		return error;
	if (made > out_capacity)
		return EOVERFLOW;
	if (made != 0)
		memcpy(out, expanded, made * sizeof(*out));
	*out_count = made;

	/* Succeeded: the longhands are declared. */
	return 0;
}

/*
 * Parses a value as a property found before (a longhand or a shorthand)
 * into longhand declarations; the same errors as css_parse_value.
 */
int
css_parse_property(
	struct css_parse *parse,
	int property,
	const struct css_token *tokens,
	size_t count,
	struct css_declaration *out,
	size_t *out_count)
{
	struct css_value value;
	int error;
	int first;
	int wide;

	/* Nothing is no value. */
	*out_count = 0;
	while (count > 0 && tokens[0].type == CSS_TOKEN_WHITESPACE) {
		tokens++;
		count--;
	}
	while (count > 0 && tokens[count - 1U].type == CSS_TOKEN_WHITESPACE)
		count--;
	if (count == 0)
		return EINVAL;

	/* The CSS-wide keywords apply to every longhand a shorthand stands for. */
	memset(&value, 0, sizeof(value));
	wide = values_keyword(values_wide, &tokens[0], &value.kind);
	if (count == 1 && wide) {
		values_wide_keyword(property, &value, out, out_count);
		return 0;
	}

	/* The shorthands. */
	switch (property) {
	case SHORT_MARGIN:
		return values_four(parse, CSS_PROP_MARGIN_TOP, tokens, count, out, out_count);
	case SHORT_PADDING:
		return values_four(parse, CSS_PROP_PADDING_TOP, tokens, count, out, out_count);
	case SHORT_BORDER_WIDTH:
		return values_four(parse, CSS_PROP_BORDER_TOP_WIDTH, tokens, count, out, out_count);
	case SHORT_BORDER_STYLE:
		return values_four(parse, CSS_PROP_BORDER_TOP_STYLE, tokens, count, out, out_count);
	case SHORT_BORDER_COLOR:
		return values_four(parse, CSS_PROP_BORDER_TOP_COLOR, tokens, count, out, out_count);
	case SHORT_BORDER:
		return values_border(parse, -1, tokens, count, out, out_count);
	case SHORT_BORDER_TOP:
		return values_border(parse, CSS_TOP, tokens, count, out, out_count);
	case SHORT_BORDER_RIGHT:
		return values_border(parse, CSS_RIGHT, tokens, count, out, out_count);
	case SHORT_BORDER_BOTTOM:
		return values_border(parse, CSS_BOTTOM, tokens, count, out, out_count);
	case SHORT_BORDER_LEFT:
		return values_border(parse, CSS_LEFT, tokens, count, out, out_count);
	case SHORT_BORDER_INLINE_START:
	case SHORT_BORDER_INLINE_END:
	case SHORT_BORDER_BLOCK_START:
	case SHORT_BORDER_BLOCK_END:
		/* A logical side: the cascade finds its physical side. */
		return values_border(parse, VALUES_LOGICAL_SIDE + property - SHORT_BORDER_INLINE_START, tokens, count, out, out_count);
	case SHORT_BACKGROUND:
		return values_background(parse, tokens, count, out, out_count);
	case SHORT_BACKGROUND_POSITION:
		return values_background_position(tokens, count, out, out_count);
	case SHORT_BACKGROUND_SIZE:
		return values_background_size(tokens, count, out, out_count);
	case SHORT_TEXT_DECORATION:
		return values_text_decoration(tokens, count, out, out_count);
	case SHORT_LIST_STYLE:
		for (first = 0; first < (int)count; first++) {
			error = values_single(parse, CSS_PROP_LIST_STYLE_TYPE, &tokens[first], 1, &value);
			if (error == 0) {
				values_add(out, out_count, CSS_PROP_LIST_STYLE_TYPE, &value);
				return 0;
			}
		}

		/* A list style without a type is not used in this pass. */
		return EINVAL;
	case SHORT_FONT:
		return css_parse_font(parse, tokens, count, out, out_count);
	case SHORT_MARGIN_INLINE:
		return values_pair(parse, CSS_PROP_MARGIN_INLINE_START, CSS_PROP_MARGIN_INLINE_END, tokens, count, out, out_count);
	case SHORT_MARGIN_BLOCK:
		return values_pair(parse, CSS_PROP_MARGIN_BLOCK_START, CSS_PROP_MARGIN_BLOCK_END, tokens, count, out, out_count);
	case SHORT_PADDING_INLINE:
		return values_pair(parse, CSS_PROP_PADDING_INLINE_START, CSS_PROP_PADDING_INLINE_END, tokens, count, out, out_count);
	case SHORT_PADDING_BLOCK:
		return values_pair(parse, CSS_PROP_PADDING_BLOCK_START, CSS_PROP_PADDING_BLOCK_END, tokens, count, out, out_count);
	case SHORT_INSET:
		return values_four(parse, CSS_PROP_TOP, tokens, count, out, out_count);
	case SHORT_INSET_INLINE:
		return values_pair(parse, CSS_PROP_INSET_INLINE_START, CSS_PROP_INSET_INLINE_END, tokens, count, out, out_count);
	case SHORT_INSET_BLOCK:
		return values_pair(parse, CSS_PROP_INSET_BLOCK_START, CSS_PROP_INSET_BLOCK_END, tokens, count, out, out_count);
	case SHORT_FLEX:
		return values_flex(parse, tokens, count, out, out_count);
	case SHORT_FLEX_FLOW:
		return values_flex_flow(parse, tokens, count, out, out_count);
	case SHORT_GAP:
		return values_pair(parse, CSS_PROP_ROW_GAP, CSS_PROP_COLUMN_GAP, tokens, count, out, out_count);
	case SHORT_BORDER_RADIUS:
		return values_border_radius(parse, tokens, count, out, out_count);
	case SHORT_RADIUS_TOP_LEFT:
		return values_corner_radius(parse, CSS_TOP_LEFT, tokens, count, out, out_count);
	case SHORT_RADIUS_TOP_RIGHT:
		return values_corner_radius(parse, CSS_TOP_RIGHT, tokens, count, out, out_count);
	case SHORT_RADIUS_BOTTOM_RIGHT:
		return values_corner_radius(parse, CSS_BOTTOM_RIGHT, tokens, count, out, out_count);
	case SHORT_RADIUS_BOTTOM_LEFT:
		return values_corner_radius(parse, CSS_BOTTOM_LEFT, tokens, count, out, out_count);
	case SHORT_OUTLINE:
		return values_outline(parse, tokens, count, out, out_count);
	case SHORT_GRID_COLUMN:
		return values_grid_pair(parse, CSS_PROP_GRID_COLUMN_START, CSS_PROP_GRID_COLUMN_END, tokens, count, out, out_count);
	case SHORT_GRID_ROW:
		return values_grid_pair(parse, CSS_PROP_GRID_ROW_START, CSS_PROP_GRID_ROW_END, tokens, count, out, out_count);
	case SHORT_GRID_AREA:
		return values_grid_area(parse, tokens, count, out, out_count);
	case SHORT_GRID_TEMPLATE:
		return values_grid_template(parse, tokens, count, out, out_count);
	case SHORT_GRID:
		return values_grid(parse, tokens, count, out, out_count);
	case SHORT_OVERFLOW:
		/* One keyword for both axes, or the horizontal then the vertical one. */
		return values_pair(parse, CSS_PROP_OVERFLOW_X, CSS_PROP_OVERFLOW_Y, tokens, count, out, out_count);
	case SHORT_TRANSFORM:
		return values_transform(tokens, count, out, out_count);
	case SHORT_TRANSFORM_ORIGIN:
		return values_transform_origin(tokens, count, CSS_PROP_TRANSFORM_ORIGIN_X, out, out_count);
	case SHORT_OBJECT_POSITION:
		return values_transform_origin(tokens, count, CSS_PROP_OBJECT_POSITION_X, out, out_count);
	case SHORT_ASPECT_RATIO:
		return values_aspect_ratio(tokens, count, out, out_count);
	case SHORT_BORDER_SPACING:
		/* One length for both directions, or across then down. */
		return values_pair(parse, CSS_PROP_BORDER_SPACING_X, CSS_PROP_BORDER_SPACING_Y, tokens, count, out, out_count);
	case SHORT_CONTAINER:
		/* container: a name, then a slash and the type (the name is not kept in this pass). */
		first = (int)values_split_at(tokens, count, CSS_TOKEN_DELIM, '/');
		if ((size_t)first < count) {
			first = (int)values_skip_space(tokens, count, (size_t)first + 1U);
			return css_parse_value_as(parse, CSS_PROP_CONTAINER_TYPE, tokens + first, count - (size_t)first, out, out_count);
		}

		/* A name alone makes a container of no type. */
		return 0;
	case SHORT_PLACE_ITEMS:
		return values_place(parse, tokens, count, CSS_PROP_ALIGN_ITEMS, CSS_PROP_JUSTIFY_ITEMS, out, out_count);
	case SHORT_PLACE_SELF:
		return values_place(parse, tokens, count, CSS_PROP_ALIGN_SELF, CSS_PROP_JUSTIFY_SELF, out, out_count);
	case SHORT_PLACE_CONTENT:
		return values_place(parse, tokens, count, CSS_PROP_ALIGN_CONTENT, CSS_PROP_JUSTIFY_CONTENT, out, out_count);
	case SHORT_POSITION_TRY:
		return values_position_try(parse, tokens, count, out, out_count);
	default:
		break;
	}

	/* A longhand. */
	error = values_single(parse, property, tokens, count, &value);
	if (error != 0)
		return error;
	values_add(out, out_count, property, &value);

	/* Succeeded: one declaration. */
	return 0;
}

/*
 * Parses a longhand's value from tokens as a given property (used by the
 * shorthands).
 */
int
css_parse_value_as(
	struct css_parse *parse,
	int property,
	const struct css_token *tokens,
	size_t count,
	struct css_declaration *out,
	size_t *out_count)
{
	struct css_value value;
	int error;

	/* Parses the value and adds the declaration. */
	memset(&value, 0, sizeof(value));
	error = values_single(parse, property, tokens, count, &value);
	if (error != 0)
		return error;
	values_add(out, out_count, property, &value);

	/* Succeeded: one declaration. */
	return 0;
}

/*
 * Parses the font shorthand: [style] [weight] size[/line-height] family.
 */
int
css_parse_font(
	struct css_parse *parse,
	const struct css_token *tokens,
	size_t count,
	struct css_declaration *out,
	size_t *out_count)
{
	struct css_value value;
	size_t index;
	int keyword;
	int found;
	int error;

	/* Style and weight keywords come before the size. */
	index = 0;
	for (;;) {
		while (index < count && tokens[index].type == CSS_TOKEN_WHITESPACE)
			index++;
		if (index >= count)
			return EINVAL;
		memset(&value, 0, sizeof(value));
		found = values_keyword(values_font_style, &tokens[index], &keyword);
		if (found && keyword != 0) {
			value.kind = CSS_VALUE_KEYWORD;
			value.keyword = keyword;
			values_add(out, out_count, CSS_PROP_FONT_STYLE, &value);
			index++;
			continue;
		}

		/* A weight: a keyword or a number from 1 to 1000. */
		found = values_keyword(values_font_weight, &tokens[index], &keyword);
		if (!found && tokens[index].type == CSS_TOKEN_NUMBER && tokens[index].number >= 1 && tokens[index].number <= 1000) {
			found = 1;
			keyword = (int)tokens[index].number;
		}

		/* The weight is declared. */
		if (found) {
			value.kind = CSS_VALUE_KEYWORD;
			value.keyword = keyword;
			values_add(out, out_count, CSS_PROP_FONT_WEIGHT, &value);
			index++;
			continue;
		}

		/* The variants this pass does not draw are read past. */
		found = values_keyword(values_font_ignored, &tokens[index], &keyword);
		if (found) {
			index++;
			continue;
		}

		/* Anything else is the size. */
		break;
	}

	/* The size, and an optional line height after a slash. */
	error = css_parse_value_as(parse, CSS_PROP_FONT_SIZE, &tokens[index], 1, out, out_count);
	if (error != 0)
		return error;
	index++;
	if (index < count && tokens[index].type == CSS_TOKEN_DELIM && tokens[index].delim == '/') {
		index++;
		while (index < count && tokens[index].type == CSS_TOKEN_WHITESPACE)
			index++;
		if (index >= count)
			return EINVAL;
		error = css_parse_value_as(parse, CSS_PROP_LINE_HEIGHT, &tokens[index], 1, out, out_count);
		if (error != 0)
			return error;
		index++;
	} else {
		/* A font shorthand without a slash resets inherited line-height to normal. */
		memset(&value, 0, sizeof(value));
		value.kind = CSS_VALUE_INITIAL;
		values_add(out, out_count, CSS_PROP_LINE_HEIGHT, &value);
	}

	/* The families take the rest. */
	while (index < count && tokens[index].type == CSS_TOKEN_WHITESPACE)
		index++;
	if (index >= count)
		return EINVAL;
	error = css_parse_value_as(parse, CSS_PROP_FONT_FAMILY, &tokens[index], count - index, out, out_count);
	if (error != 0)
		return error;

	/* Succeeded: the longhands are declared. */
	return 0;
}

/*
 * Parses a color: a named color, transparent, currentcolor, #rgb, #rgba,
 * #rrggbb, #rrggbbaa, rgb(), rgba(), hsl() or hsla().
 *
 * currentcolor is reported as 0x00000001, a value no real color has with
 * zero alpha, which the cascade replaces with the color property.
 */
int
css_parse_color(
	const struct css_token *tokens,
	size_t count,
	uint32_t *color)
{
	const struct css_token *token;
	uint32_t value;
	size_t index;
	size_t used;
	int keyword;
	int digit;
	int same;
	int hsl;

	/* Skips leading whitespace. */
	while (count > 0 && tokens[0].type == CSS_TOKEN_WHITESPACE) {
		tokens++;
		count--;
	}
	while (count > 0 && tokens[count - 1U].type == CSS_TOKEN_WHITESPACE)
		count--;

	/* Nothing is not a color. */
	if (count == 0)
		return EINVAL;
	token = &tokens[0];

	/* Keywords and named colors. */
	if (token->type == CSS_TOKEN_IDENT) {
		if (count != 1U)
			return EINVAL;
		same = values_keyword(values_color_keywords, token, &keyword);
		if (same) {
			*color = (uint32_t)keyword;
			return 0;
		}

		/* The named colors. */
		for (index = 0; values_colors[index].name != NULL; index++) {
			same = css_ident_equal(token, values_colors[index].name);
			if (same) {
				*color = 0xff000000U | values_colors[index].rgb;
				return 0;
			}
		}

		/* An unknown word is not a color. */
		return EINVAL;
	}

	/* Hex colors of 3, 4, 6 or 8 digits. */
	if (token->type == CSS_TOKEN_HASH) {
		if (count != 1U)
			return EINVAL;
		value = 0;
		for (index = 0; index < token->length; index++) {
			digit = values_hex_digit(token->text[index]);
			if (digit < 0)
				return EINVAL;
			value = value * 16U + (uint32_t)digit;
		}

		/* The number of digits picks the form. */
		switch (token->length) {
		case 3:
			*color = 0xff000000U |
			    ((value >> 8) & 0xfU) * 0x110000U |
			    ((value >> 4) & 0xfU) * 0x1100U |
			    (value & 0xfU) * 0x11U;
			return 0;
		case 4:
			*color = ((value & 0xfU) * 0x11U) << 24 |
			    ((value >> 12) & 0xfU) * 0x110000U |
			    ((value >> 8) & 0xfU) * 0x1100U |
			    ((value >> 4) & 0xfU) * 0x11U;
			return 0;
		case 6:
			*color = 0xff000000U | value;
			return 0;
		case 8:
			*color = (value << 24) | (value >> 8);
			return 0;
		default:
			return EINVAL;
		}
	}

	/* rgb() and rgba(). */
	if (token->type == CSS_TOKEN_FUNCTION) {
		same = css_ident_equal(token, "rgb");
		if (!same)
			same = css_ident_equal(token, "rgba");
		hsl = css_ident_equal(token, "hsl") || css_ident_equal(token, "hsla");
		if (!same && !hsl)
			return EINVAL;
		if (count < 2U || tokens[count - 1U].type != CSS_TOKEN_CLOSE_PAREN)
			return EINVAL;
		used = values_function_length(tokens, count);
		if (used != count)
			return EINVAL;
		if (hsl)
			return values_hsl_function(tokens + 1, count - 1U, color);
		return values_rgb_function(tokens + 1, count - 1U, color);
	}

	/* Anything else is not a color. */
	return EINVAL;
}

/*
 * Serializes one declared numeric component using the native legacy scalar representation.
 * Nonnumeric components remain unhandled, and the caller retains their original grammar and spelling.
 */
int
css_number_serialize(
	const uint16_t *units,
	size_t length,
	struct wb_units *out,
	int *handled)
{
	struct wb_arena arena;
	struct css_token *tokens;
	char text[64];
	double number;
	size_t count;
	size_t start;
	size_t end;
	size_t index;
	unsigned char unit;
	int decimal;
	int numeric;
	int written;
	int status;

	/* The production tokenizer supplies exact declared doubles and skips CSS comments. */
	*handled = 0;
	wb_arena_init(&arena, 0);
	status = css_tokenize(&arena, units, length, &tokens, &count);
	if (status != 0)
		goto cleanup;
	start = 0;
	end = count;
	if (end != 0U &&
	    tokens[end - 1U].type == CSS_TOKEN_EOF)
		end--;

	/* Only one numeric token surrounded by whitespace belongs to this scalar component path. */
	while (start < end &&
	       tokens[start].type == CSS_TOKEN_WHITESPACE)
		start++;
	while (end > start &&
	       tokens[end - 1U].type == CSS_TOKEN_WHITESPACE)
		end--;
	if (end - start != 1U)
		goto cleanup;
	if (tokens[start].type != CSS_TOKEN_NUMBER)
		goto cleanup;

	/* Declared precision remains double, while the native supported numeric range is bounded by float magnitude. */
	number = tokens[start].number;
	if (number > FLT_MAX)
		number = FLT_MAX;
	if (number < -FLT_MAX)
		number = -FLT_MAX;
	if (number == 0.0)
		number = 0.0;
	written = snprintf(text, sizeof(text), "%.6g", number);
	if (written < 0) {
		status = EIO;
		goto cleanup;
	}

	/* The fixed buffer must contain the complete finite scalar representation. */
	if ((size_t)written >= sizeof(text)) {
		status = EOVERFLOW;
		goto cleanup;
	}

	/* CSS uses a dot regardless of the process locale; a printf decimal separator is its only nondigit punctuation. */
	decimal = 0;
	for (index = 0; index < (size_t)written; index++) {
		unit = (unsigned char)text[index];
		numeric = 0;
		if (unit >= '0' &&
		    unit <= '9')
			numeric = 1;
		if (unit == 'e' ||
		    unit == 'E' ||
		    unit == '+' ||
		    unit == '-')
			numeric = 1;
		if (!numeric) {
			if (decimal)
				continue;
			unit = '.';
			decimal = 1;
		}

		/* Append ASCII digits, signs, exponent marker and the normalized decimal point. */
		status = wb_units_append_code_point(out, unit);
		if (status != 0)
			goto cleanup;
	}

	/* A complete numeric serialization is now available to the caller. */
	*handled = 1;

cleanup:
	/* No tokenizer source slice survives its arena. */
	wb_arena_release(&arena);
	if (status != 0)
		return status;

	/* Succeeded: handled distinguishes a scalar result from an untouched nonnumeric component. */
	return 0;
}

/* Parses one text-decoration-line value with the production CSS tokenizer. */
int
css_decoration_line_parse(
	const uint16_t *units,
	size_t length,
	int *lines)
{
	struct css_value value;
	struct css_token *tokens;
	struct wb_arena arena;
	size_t count;
	size_t start;
	size_t end;
	int error;

	wb_arena_init(&arena, 0);
	error = css_tokenize(&arena, units, length, &tokens, &count);
	if (error != 0) {
		wb_arena_release(&arena);
		return error;
	}
	if (count == 0U) {
		wb_arena_release(&arena);
		return EINVAL;
	}
	start = 0;
	end = count - 1U;
	while (start < end && tokens[start].type == CSS_TOKEN_WHITESPACE)
		start++;
	while (end > start && tokens[end - 1U].type == CSS_TOKEN_WHITESPACE)
		end--;
	if (start == end)
		error = EINVAL;
	else
		error = values_decoration_line(tokens + start, end - start, &value);
	if (error == 0)
		*lines = value.keyword;
	wb_arena_release(&arena);
	return error;
}

/* Writes the specified decoration lines in CSS's canonical grammar order. */
int
css_decoration_line_serialize(
	int lines,
	struct wb_units *out)
{
	static const struct {
		int bit;
		const char *name;
	} names[] = {
		{ CSS_DECORATION_UNDERLINE, "underline" },
		{ CSS_DECORATION_OVERLINE, "overline" },
		{ CSS_DECORATION_LINE_THROUGH, "line-through" },
		{ CSS_DECORATION_BLINK, "blink" }
	};
	const char *single;
	size_t index;
	int written;
	int error;

	single = NULL;
	if (lines == 0)
		single = "none";
	if (lines == CSS_DECORATION_SPELLING_ERROR)
		single = "spelling-error";
	if (lines == CSS_DECORATION_GRAMMAR_ERROR)
		single = "grammar-error";
	if (single != NULL)
		return wb_utf8_to_units((const unsigned char *)single, strlen(single), out);

	written = 0;
	for (index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
		if ((lines & names[index].bit) == 0)
			continue;
		if (written) {
			error = wb_utf8_to_units((const unsigned char *)" ", 1, out);
			if (error != 0)
				return error;
		}
		error = wb_utf8_to_units((const unsigned char *)names[index].name, strlen(names[index].name), out);
		if (error != 0)
			return error;
		written = 1;
	}

	/* Succeeded: ordinary line names have one canonical order. */
	return 0;
}

/* The physical property whose grammar a logical side or size takes (other properties their own). */
static int
values_physical_grammar(
	int property)
{
	/* Each logical group, by its first physical property. */
	if (property >= CSS_PROP_PADDING_INLINE_START && property <= CSS_PROP_PADDING_BLOCK_END)
		return CSS_PROP_PADDING_TOP;
	if (property >= CSS_PROP_INSET_INLINE_START && property <= CSS_PROP_INSET_BLOCK_END)
		return CSS_PROP_TOP;
	if (property >= CSS_PROP_BORDER_INLINE_START_WIDTH && property <= CSS_PROP_BORDER_BLOCK_END_WIDTH)
		return CSS_PROP_BORDER_TOP_WIDTH;
	if (property >= CSS_PROP_BORDER_INLINE_START_STYLE && property <= CSS_PROP_BORDER_BLOCK_END_STYLE)
		return CSS_PROP_BORDER_TOP_STYLE;
	if (property >= CSS_PROP_BORDER_INLINE_START_COLOR && property <= CSS_PROP_BORDER_BLOCK_END_COLOR)
		return CSS_PROP_BORDER_TOP_COLOR;
	if (property == CSS_PROP_INLINE_SIZE || property == CSS_PROP_BLOCK_SIZE)
		return CSS_PROP_WIDTH;
	if (property == CSS_PROP_MIN_INLINE_SIZE || property == CSS_PROP_MIN_BLOCK_SIZE)
		return CSS_PROP_MIN_WIDTH;
	if (property == CSS_PROP_MAX_INLINE_SIZE || property == CSS_PROP_MAX_BLOCK_SIZE)
		return CSS_PROP_MAX_WIDTH;

	/* Any other property is its own grammar. */
	return property;
}

/* Looks a keyword up in a table; returns 1 and its value when it is there. */
static int
values_keyword(
	const struct values_keyword *table,
	const struct css_token *token,
	int *value)
{
	size_t index;
	int same;

	/* Only identifiers are keywords. */
	if (token->type != CSS_TOKEN_IDENT)
		return 0;

	/* Compares with each keyword. */
	for (index = 0; table[index].name != NULL; index++) {
		same = css_ident_equal(token, table[index].name);
		if (same) {
			*value = table[index].value;
			return 1;
		}
	}

	/* The word is not one of the table's. */
	return 0;
}

/* Looks a dimension's unit up; returns 1 and the unit when it is a length unit. */
static int
values_unit(
	const struct css_token *token,
	int *unit)
{
	size_t index;
	int same;

	/* Only dimensions have units. */
	if (token->type != CSS_TOKEN_DIMENSION)
		return 0;

	/* Compares the unit with each length unit. */
	for (index = 0; values_units[index].name != NULL; index++) {
		same = css_ident_equal(token, values_units[index].name);
		if (same) {
			*unit = values_units[index].value;
			return 1;
		}
	}

	/* The unit is not a length unit. */
	return 0;
}

/* Parses a length or percentage (and, when allowed, auto, none and normal). */
static int
values_length(
	const struct css_token *token,
	int allow_keywords,
	struct css_value *value)
{
	int found;

	/* Keywords. */
	value->kind = CSS_VALUE_LENGTH;
	if (token->type == CSS_TOKEN_IDENT && allow_keywords) {
		found = values_keyword(values_length_keywords, token, &value->keyword);
		if (!found)
			return EINVAL;
		value->kind = CSS_VALUE_KEYWORD;
		return 0;
	}

	/* Zero, percentages and dimensions. */
	if (token->type == CSS_TOKEN_NUMBER && token->number == 0) {
		value->number = 0;
		value->unit = CSS_DUNIT_PX;
		return 0;
	}

	/* A percentage. */
	if (token->type == CSS_TOKEN_PERCENTAGE) {
		value->number = (float)token->number;
		value->unit = CSS_DUNIT_PERCENT;
		return 0;
	}

	/* Otherwise a dimension with a known unit. */
	if (token->type != CSS_TOKEN_DIMENSION)
		return EINVAL;
	value->number = (float)token->number;
	found = values_unit(token, &value->unit);
	if (!found)
		return EINVAL;

	/* Succeeded: the length and its unit. */
	return 0;
}

/* Parses the nonnegative width grammar and the separate content basis. */
static int
values_flex_basis(
	const struct css_token *token,
	struct css_value *value)
{
	int content;
	int error;

	/* Preserves content rather than borrowing the preferred main size. */
	content = css_ident_equal(token, "content");
	if (content) {
		value->kind = CSS_VALUE_KEYWORD;
		value->keyword = CSS_UNIT_CONTENT;
		return 0;
	}

	/* Parses the existing width-like keywords and literal lengths. */
	error = values_length(token, 1, value);
	if (error != 0)
		return error;

	/* The generic length keywords none and normal are not flex bases. */
	if (value->kind == CSS_VALUE_KEYWORD) {
		/* These generic keywords do not belong to the width grammar. */
		if (value->keyword == CSS_UNIT_NONE || value->keyword == CSS_UNIT_NORMAL)
			return EINVAL;
	} else {
		/* Literal negatives are invalid; math functions use a separate parser. */
		if (value->number < 0)
			return EINVAL;
	}

	/* Succeeded: the token is one accepted flex basis. */
	return 0;
}

/* Parses a longhand's value. */
static int
values_single(
	struct css_parse *parse,
	int property,
	const struct css_token *tokens,
	size_t count,
	struct css_value *value)
{
	const struct values_keyword *table;
	const struct css_token *parts[2];
	size_t used;
	size_t words;
	size_t word;
	int keyword;
	int is_auto;
	int found;
	int takes_length;
	int error;
	int is_baseline;
	int is_first;
	int is_last;
	int is_safe;
	int is_unsafe;

	/* A logical side or size takes the grammar of a physical one. */
	property = values_physical_grammar(property);

	/* Skips surrounding whitespace. */
	while (count > 0 &&
	       tokens[0].type == CSS_TOKEN_WHITESPACE) {
		tokens++;
		count--;
	}

	/* Discard trailing whitespace before validating the component count. */
	while (count > 0 &&
	       tokens[count - 1U].type == CSS_TOKEN_WHITESPACE)
		count--;
	if (count == 0)
		return EINVAL;
	memset(value, 0, sizeof(*value));

	/* An inset or a size with anchor functions is kept as a sum the layout finishes. */
	if (((property >= CSS_PROP_TOP && property <= CSS_PROP_LEFT) ||
	    (property >= CSS_PROP_WIDTH && property <= CSS_PROP_MAX_HEIGHT)) && css_anchor_mentioned(tokens, count)) {
		error = css_anchor_parse(parse, tokens, count, property >= CSS_PROP_WIDTH && property <= CSS_PROP_MAX_HEIGHT,
		    &value->anchor);
		if (error != 0)
			return error;
		value->kind = CSS_VALUE_ANCHOR;
		return 0;
	}

	/* anchor-name and anchor-scope: none (anchor-scope also all) or names. */
	if (property == CSS_PROP_ANCHOR_NAME || property == CSS_PROP_ANCHOR_SCOPE) {
		if (count == 1 && css_ident_equal(&tokens[0], "none")) {
			value->kind = CSS_VALUE_KEYWORD;
			value->keyword = 0;
			return 0;
		}
		if (property == CSS_PROP_ANCHOR_SCOPE && count == 1 && css_ident_equal(&tokens[0], "all")) {
			value->kind = CSS_VALUE_KEYWORD;
			value->keyword = 1;
			return 0;
		}
		error = css_anchor_names_parse(parse, tokens, count, &value->anchor_names);
		if (error != 0)
			return error;
		value->kind = CSS_VALUE_ANCHOR_NAMES;
		return 0;
	}

	/* position-area: none or its keywords. */
	if (property == CSS_PROP_POSITION_AREA) {
		error = css_position_area_parse(tokens, count, &keyword);
		if (error != 0)
			return error;
		value->kind = CSS_VALUE_NUMBER;
		value->number = (float)keyword;
		return 0;
	}

	/* position-try-fallbacks, position-try-order and position-visibility. */
	if (property == CSS_PROP_POSITION_TRY_FALLBACKS) {
		error = css_try_fallbacks_parse(parse, tokens, count, &value->try_list);
		if (error != 0)
			return error;
		value->kind = CSS_VALUE_TRY;
		return 0;
	}
	if (property == CSS_PROP_POSITION_TRY_ORDER || property == CSS_PROP_POSITION_VISIBILITY) {
		if (property == CSS_PROP_POSITION_TRY_ORDER)
			error = css_try_order_parse(tokens, count, &keyword);
		else
			error = css_position_visibility_parse(tokens, count, &keyword);
		if (error != 0)
			return error;
		value->kind = CSS_VALUE_NUMBER;
		value->number = (float)keyword;
		return 0;
	}

	/* position-anchor: auto or a name. */
	if (property == CSS_PROP_POSITION_ANCHOR) {
		if (count != 1)
			return EINVAL;
		if (css_ident_equal(&tokens[0], "auto")) {
			value->kind = CSS_VALUE_KEYWORD;
			value->keyword = 0;
			return 0;
		}
		if (tokens[0].type != CSS_TOKEN_IDENT || tokens[0].length < 3 || tokens[0].text[0] != '-' || tokens[0].text[1] != '-')
			return EINVAL;
		value->ident = vm_atom_from_units(parse->heap, tokens[0].text, tokens[0].length);
		if (value->ident == NULL)
			return ENOMEM;
		value->kind = CSS_VALUE_KEYWORD;
		value->keyword = 1;
		return 0;
	}

	/* Decoration lines form an unordered list of distinct keywords. */
	if (property == CSS_PROP_TEXT_DECORATION_LINE) {
		/* Parse the selected component with its established native grammar. */
		error = values_decoration_line(tokens, count, value);
		if (error != 0)
			return error;

		/* Succeeded: the selected native scalar component was parsed. */
		return 0;
	}

	/* Thickness accepts one automatic, font-derived or nonnegative length component. */
	if (property == CSS_PROP_TEXT_DECORATION_THICKNESS) {
		if (count != 1U)
			return EINVAL;
		is_auto = css_ident_equal(&tokens[0], "auto");
		if (is_auto) {
			value->kind = CSS_VALUE_KEYWORD;
			value->keyword = 0;
			return 0;
		}

		/* The font may provide the thickness instead of a numeric length. */
		found = css_ident_equal(&tokens[0], "from-font");
		if (found) {
			value->kind = CSS_VALUE_KEYWORD;
			value->keyword = 1;
			return 0;
		}

		/* A remaining thickness component must be a supported nonnegative length. */
		error = values_length(&tokens[0], 0, value);
		if (error != 0 ||
		    value->number < 0)
			return EINVAL;
		return 0;
	}

	/* Families take a list; colors may be a function of several tokens. */
	if (property == CSS_PROP_FONT_FAMILY) {
		/* Parse the selected component with its established native grammar. */
		error = values_families(parse, tokens, count, value);
		if (error != 0)
			return error;

		/* Succeeded: the selected native scalar component was parsed. */
		return 0;
	}

	/* Color properties share the native color parser. */
	if (property == CSS_PROP_COLOR ||
	    property == CSS_PROP_BACKGROUND_COLOR ||
	    property == CSS_PROP_TEXT_DECORATION_COLOR ||
	    (property >= CSS_PROP_BORDER_TOP_COLOR && property <= CSS_PROP_BORDER_LEFT_COLOR)) {
		error = css_parse_color(tokens, count, &value->color);
		if (error != 0)
			return error;
		value->kind = CSS_VALUE_COLOR;
		return 0;
	}

	/* An image: url(...) or none. */
	if (property == CSS_PROP_BACKGROUND_IMAGE) {
		/* Parse the selected component with its established native grammar. */
		error = values_url(parse, tokens, count, value);
		if (error != 0)
			return error;

		/* Succeeded: the selected native scalar component was parsed. */
		return 0;
	}

	/* background-repeat takes one keyword or one for each axis. */
	if (property == CSS_PROP_BACKGROUND_REPEAT) {
		parts[0] = &tokens[0];
		parts[1] = &tokens[count - 1U];
		used = 2;
		if (count == 1)
			used = 1;
		error = values_repeat_parts(parts, used, value);
		if (error != 0)
			return error;

		/* Succeeded: the repeat component is available on both axes. */
		return 0;
	}

	/* background-attachment takes one scrolling keyword. */
	if (property == CSS_PROP_BACKGROUND_ATTACHMENT) {
		found = values_keyword(values_attachment, &tokens[0], &keyword);
		if (!found ||
		    count != 1)
			return EINVAL;

		/* Publish the validated native keyword after its property-specific restrictions. */
		value->kind = CSS_VALUE_KEYWORD;
		value->keyword = keyword;
		return 0;
	}

	/* content takes a list of strings and functions. */
	if (property == CSS_PROP_CONTENT) {
		/* Parse the selected component with its established native grammar. */
		error = values_content(parse, tokens, count, value);
		if (error != 0)
			return error;

		/* Succeeded: the selected native scalar component was parsed. */
		return 0;
	}

	/* box-shadow takes a list of shadows. */
	if (property == CSS_PROP_BOX_SHADOW) {
		/* Parse the selected component with its established native grammar. */
		error = values_box_shadow(parse, tokens, count, value);
		if (error != 0)
			return error;

		/* Succeeded: the selected native scalar component was parsed. */
		return 0;
	}

	/* Text shadows retain the established multi-component grammar. */
	if (property == CSS_PROP_TEXT_SHADOW) {
		/* Parse the selected component with its established native grammar. */
		error = values_text_shadow(parse, tokens, count, value);
		if (error != 0)
			return error;

		/* Succeeded: the selected native scalar component was parsed. */
		return 0;
	}

	/* clip-path takes a shape. */
	if (property == CSS_PROP_CLIP_PATH) {
		/* Parse the selected component with its established native grammar. */
		error = values_clip_path(parse, tokens, count, value);
		if (error != 0)
			return error;

		/* Succeeded: the selected native scalar component was parsed. */
		return 0;
	}

	/* clip takes auto or rect(). */
	if (property == CSS_PROP_CLIP) {
		/* Parse the selected component with its established native grammar. */
		error = values_clip(parse, tokens, count, value);
		if (error != 0)
			return error;

		/* Succeeded: the selected native scalar component was parsed. */
		return 0;
	}

	/* A grid template takes a list of tracks. */
	if (property == CSS_PROP_GRID_TEMPLATE_COLUMNS ||
	    property == CSS_PROP_GRID_TEMPLATE_ROWS) {
		/* Parse the selected component with its established native grammar. */
		error = values_tracks(parse, tokens, count, value);
		if (error != 0)
			return error;

		/* Succeeded: the selected native scalar component was parsed. */
		return 0;
	}

	/* Named grid areas are strings of cells. */
	if (property == CSS_PROP_GRID_TEMPLATE_AREAS) {
		/* Parse the selected component with its established native grammar. */
		error = values_areas(parse, tokens, count, value);
		if (error != 0)
			return error;

		/* Succeeded: the selected native scalar component was parsed. */
		return 0;
	}

	/* Automatic grid placement has its own keyword grammar. */
	if (property == CSS_PROP_GRID_AUTO_FLOW) {
		/* Parse the selected component with its established native grammar. */
		error = values_grid_auto_flow(tokens, count, value);
		if (error != 0)
			return error;

		/* Succeeded: the selected native scalar component was parsed. */
		return 0;
	}

	/* Implicit track sizes retain their existing list grammar. */
	if (property == CSS_PROP_GRID_AUTO_COLUMNS ||
	    property == CSS_PROP_GRID_AUTO_ROWS) {
		/* Parse the selected component with its established native grammar. */
		error = values_auto_tracks(parse, tokens, count, value);
		if (error != 0)
			return error;

		/* Succeeded: the selected native scalar component was parsed. */
		return 0;
	}

	/* A grid line: auto, a number, or span and a number. */
	if (property >= CSS_PROP_GRID_COLUMN_START &&
	    property <= CSS_PROP_GRID_ROW_END) {
		/* Parse the selected component with its established native grammar. */
		error = values_grid_line(parse, tokens, count, value);
		if (error != 0)
			return error;

		/* Succeeded: the selected native scalar component was parsed. */
		return 0;
	}

	/* The outline's width is a border's. */
	if (property == CSS_PROP_OUTLINE_WIDTH) {
		/* Parse the selected component with its established native grammar. */
		error = values_single(parse, CSS_PROP_BORDER_TOP_WIDTH, tokens, count, value);
		if (error != 0)
			return error;

		/* Succeeded: the selected native scalar component was parsed. */
		return 0;
	}

	/* Its style is a border's, and auto a solid one. */
	if (property == CSS_PROP_OUTLINE_STYLE) {
		is_auto = css_ident_equal(&tokens[0], "auto");
		if (count == 1 &&
		    is_auto) {
			value->kind = CSS_VALUE_KEYWORD;
			value->keyword = CSS_BORDER_SOLID;
			return 0;
		}

		/* Any other style uses the existing border-style grammar. */
		error = values_single(parse, CSS_PROP_BORDER_TOP_STYLE, tokens, count, value);
		if (error != 0)
			return error;

		/* Succeeded: the selected native scalar component was parsed. */
		return 0;
	}

	/* Its color is a border's, and invert the current color. */
	if (property == CSS_PROP_OUTLINE_COLOR) {
		is_auto = css_ident_equal(&tokens[0], "invert");
		if (count == 1 &&
		    is_auto) {
			value->kind = CSS_VALUE_COLOR;
			value->color = CSS_CURRENT_COLOR;
			return 0;
		}

		/* Any other color uses the existing border-color grammar. */
		error = values_single(parse, CSS_PROP_BORDER_TOP_COLOR, tokens, count, value);
		if (error != 0)
			return error;

		/* Succeeded: the selected native scalar component was parsed. */
		return 0;
	}

	/* A length may be a calculation, which is a function of several tokens. */
	takes_length = values_takes_length(property);
	if (takes_length &&
	    tokens[0].type == CSS_TOKEN_FUNCTION) {
		used = values_function_length(tokens, count);
		if (used == count) {
			error = values_calc(parse, property, tokens, count, value);
			if (error != 0)
				return error;

			/* Succeeded: the existing calculation grammar parsed the whole component. */
			return 0;
		}
	}

	/* Alignment properties also take "first baseline", "last baseline" and an overflow position. */
	if (count > 1U &&
	    (property == CSS_PROP_JUSTIFY_CONTENT ||
	     property == CSS_PROP_JUSTIFY_ITEMS ||
	     property == CSS_PROP_ALIGN_ITEMS || property == CSS_PROP_ALIGN_CONTENT ||
	     property == CSS_PROP_ALIGN_SELF || property == CSS_PROP_JUSTIFY_SELF)) {
		/* Read exactly two alignment identifiers, ignoring whitespace tokens. */
		words = 0;
		parts[0] = NULL;
		parts[1] = NULL;
		for (word = 0; word < count; word++) {
			if (tokens[word].type == CSS_TOKEN_WHITESPACE)
				continue;
			if (tokens[word].type != CSS_TOKEN_IDENT ||
			    words >= 2U)
				return EINVAL;
			parts[words] = &tokens[word];
			words++;
		}

		/* A two-word alignment may now be classified without consuming other tokens. */
		if (words != 2U)
			return EINVAL;

		/* Baseline positions retain the original short-circuit order of keyword classification. */
		is_baseline = css_ident_equal(parts[1], "baseline");
		is_first = 0;
		is_last = 0;
		if (is_baseline) {
			is_first = css_ident_equal(parts[0], "first");
			if (!is_first)
				is_last = css_ident_equal(parts[0], "last");
		}

		/* The last baseline is the end edge marked as a last baseline; the first one remains a baseline. */
		if (is_baseline &&
		    (is_first ||
		     is_last)) {
			value->kind = CSS_VALUE_KEYWORD;
			is_last = css_ident_equal(parts[0], "last");
			value->keyword = CSS_ALIGN_BASELINE;
			if (is_last)
				value->keyword = CSS_ALIGN_END | 0x2000;

			/* Succeeded: the two-word baseline position is represented. */
			return 0;
		}

		/* Only safe or unsafe can prefix the remaining single-position parser. */
		is_safe = css_ident_equal(parts[0], "safe");
		if (!is_safe) {
			is_unsafe = css_ident_equal(parts[0], "unsafe");
			if (!is_unsafe)
				return EINVAL;
		}

		/* A failed position parse cannot inspect or modify its result keyword. */
		error = values_single(parse, property, parts[1], 1U, value);
		if (error != 0)
			return error;
		is_safe = css_ident_equal(parts[0], "safe");
		if (is_safe)
			value->keyword |= 0x1000;

		/* Succeeded: the original safe-position flag is retained. */
		return 0;
	}

	/* The other properties take one token. */
	if (count != 1)
		return EINVAL;

	/* One axis of background-position: a keyword or a length. */
	if (property == CSS_PROP_BACKGROUND_POSITION_X ||
	    property == CSS_PROP_BACKGROUND_POSITION_Y) {
		/* Parse the selected component with its established native grammar. */
		error = values_position_part(&tokens[0], value);
		if (error != 0)
			return error;

		/* Succeeded: the selected native scalar component was parsed. */
		return 0;
	}

	/* The keyword properties. */
	table = NULL;
	switch (property) {
	case CSS_PROP_DISPLAY:
		table = values_display;
		break;
	case CSS_PROP_POSITION:
		table = values_position;
		break;
	case CSS_PROP_FLOAT:
		table = values_float;
		break;
	case CSS_PROP_CLEAR:
		table = values_clear;
		break;
	case CSS_PROP_OVERFLOW:
	case CSS_PROP_OVERFLOW_X:
	case CSS_PROP_OVERFLOW_Y:
	case CSS_PROP_OVERFLOW_INLINE:
	case CSS_PROP_OVERFLOW_BLOCK:
		table = values_overflow;
		break;
	case CSS_PROP_VISIBILITY:
		table = values_visibility;
		break;
	case CSS_PROP_BORDER_TOP_STYLE:
	case CSS_PROP_BORDER_RIGHT_STYLE:
	case CSS_PROP_BORDER_BOTTOM_STYLE:
	case CSS_PROP_BORDER_LEFT_STYLE:
		table = values_border_style;
		break;
	case CSS_PROP_FONT_STYLE:
		table = values_font_style;
		break;
	case CSS_PROP_TEXT_ALIGN:
		table = values_text_align;
		break;
	case CSS_PROP_DIRECTION:
		table = values_direction;
		break;
	case CSS_PROP_WRITING_MODE:
		table = values_writing_mode;
		break;
	case CSS_PROP_TEXT_ORIENTATION:
		table = values_text_orientation;
		break;
	case CSS_PROP_CONTAINER_TYPE:
		table = values_container_type;
		break;
	case CSS_PROP_OBJECT_FIT:
		table = values_object_fit;
		break;
	case CSS_PROP_BORDER_COLLAPSE:
		table = values_border_collapse;
		break;
	case CSS_PROP_CURSOR:
		table = values_cursor;
		break;
	case CSS_PROP_TEXT_TRANSFORM:
		/* Use the exact four native computed transform keywords. */
		table = values_text_transform;
		break;
	case CSS_PROP_WHITE_SPACE:
		table = values_white_space;
		break;
	case CSS_PROP_TEXT_DECORATION_STYLE:
		table = values_decoration_styles;
		break;
	case CSS_PROP_TEXT_DECORATION_SKIP_INK:
		table = values_decoration_skip_inks;
		break;
	case CSS_PROP_LIST_STYLE_TYPE:
		table = values_list_style;
		break;
	case CSS_PROP_BOX_SIZING:
		table = values_box_sizing;
		break;
	case CSS_PROP_FLEX_DIRECTION:
		table = values_flex_direction;
		break;
	case CSS_PROP_FLEX_WRAP:
		table = values_flex_wrap;
		break;
	case CSS_PROP_JUSTIFY_CONTENT:
	case CSS_PROP_JUSTIFY_ITEMS:
	case CSS_PROP_ALIGN_ITEMS:
	case CSS_PROP_ALIGN_CONTENT:
	case CSS_PROP_ALIGN_SELF:
	case CSS_PROP_JUSTIFY_SELF:
		table = values_align;
		break;
	default:
		break;
	}

	/* A keyword property takes a keyword of its table. */
	if (table != NULL) {
		found = values_keyword(table, &tokens[0], &keyword);
		if (!found)
			return EINVAL;
		/* Physical left and right belong to justify-content, never align-* properties. */
		if ((property == CSS_PROP_ALIGN_ITEMS ||
		     property == CSS_PROP_ALIGN_CONTENT ||
		     property == CSS_PROP_ALIGN_SELF) &&
		    (keyword == CSS_ALIGN_LEFT || keyword == CSS_ALIGN_RIGHT))
			return EINVAL;
		if (property == CSS_PROP_JUSTIFY_ITEMS &&
		    keyword == CSS_ALIGN_AUTO)
			return EINVAL;
		/* anchor-center is a self-alignment. */
		if (table == values_align && keyword == CSS_ALIGN_ANCHOR_CENTER &&
		    property != CSS_PROP_ALIGN_SELF && property != CSS_PROP_JUSTIFY_SELF)
			return EINVAL;
		if (property == CSS_PROP_JUSTIFY_CONTENT ||
		    property == CSS_PROP_ALIGN_ITEMS ||
		    property == CSS_PROP_ALIGN_CONTENT) {
			if (keyword == CSS_ALIGN_SELF_START)
				keyword = CSS_ALIGN_START;
			if (keyword == CSS_ALIGN_SELF_END)
				keyword = CSS_ALIGN_END;
		}

		/* Publish the validated native keyword after its property-specific restrictions. */
		value->kind = CSS_VALUE_KEYWORD;
		value->keyword = keyword;
		return 0;
	}

	/* vertical-align: a keyword, or a length or percentage that raises the box. */
	if (property == CSS_PROP_VERTICAL_ALIGN) {
		found = values_keyword(values_vertical_align, &tokens[0], &keyword);
		if (found) {
			value->kind = CSS_VALUE_KEYWORD;
			value->keyword = keyword;
			return 0;
		}

		/* A length or a percentage. */
		error = values_length(&tokens[0], 0, value);
		return error;
	}

	/* opacity: a number or a percentage, held between 0 and 1. */
	if (property == CSS_PROP_OPACITY) {
		value->kind = CSS_VALUE_NUMBER;
		if (tokens[0].type == CSS_TOKEN_NUMBER) {
			value->number = (float)tokens[0].number;
		} else if (tokens[0].type == CSS_TOKEN_PERCENTAGE) {
			value->number = (float)tokens[0].number / 100.0f;
		} else {
			return EINVAL;
		}

		/* The number, held in range. */
		if (value->number < 0)
			value->number = 0;
		if (value->number > 1)
			value->number = 1;
		return 0;
	}

	/* A corner's radius on one axis: a length or a percentage that is not negative. */
	if (property >= CSS_PROP_RADIUS_TOP_LEFT_X &&
	    property <= CSS_PROP_RADIUS_BOTTOM_LEFT_Y) {
		error = values_length(&tokens[0], 0, value);
		if (error != 0)
			return error;
		if (value->number < 0)
			return EINVAL;
		return 0;
	}

	/* text-indent, outline-offset and border-spacing: a length. */
	if (property == CSS_PROP_TEXT_INDENT ||
	    property == CSS_PROP_OUTLINE_OFFSET ||
	    property == CSS_PROP_BORDER_SPACING_X || property == CSS_PROP_BORDER_SPACING_Y) {
		/* Parse the selected component with its established native grammar. */
		error = values_length(&tokens[0], 0, value);
		if (error != 0)
			return error;

		/* Succeeded: the selected native scalar component was parsed. */
		return 0;
	}

	/* flex-grow and flex-shrink: a number that is not negative. */
	if (property == CSS_PROP_FLEX_GROW ||
	    property == CSS_PROP_FLEX_SHRINK) {
		if (tokens[0].type != CSS_TOKEN_NUMBER ||
		    tokens[0].number < 0)
			return EINVAL;
		value->kind = CSS_VALUE_NUMBER;
		if (tokens[0].number > FLT_MAX) {
			value->number = FLT_MAX;
		} else {
			value->number = (float)tokens[0].number;
		}

		/* Succeeded: the supported factor is finite without rounding its declared source. */
		return 0;
	}

	/* order: an integer. */
	if (property == CSS_PROP_ORDER) {
		if (tokens[0].type != CSS_TOKEN_NUMBER ||
		    !tokens[0].integer)
			return EINVAL;
		value->kind = CSS_VALUE_NUMBER;
		value->number = (float)tokens[0].number;
		return 0;
	}

	/* The gaps: normal (none) or a length. */
	if (property == CSS_PROP_ROW_GAP ||
	    property == CSS_PROP_COLUMN_GAP) {
		is_auto = css_ident_equal(&tokens[0], "normal");
		if (is_auto) {
			value->kind = CSS_VALUE_LENGTH;
			value->number = 0;
			value->unit = CSS_DUNIT_PX;
			return 0;
		}

		/* A length. */
		/* Parse the selected component with its established native grammar. */
		error = values_length(&tokens[0], 0, value);
		if (error != 0)
			return error;

		/* Succeeded: the selected native scalar component was parsed. */
		return 0;
	}

	/* Separates the content basis from the width-like keywords and lengths. */
	if (property == CSS_PROP_FLEX_BASIS) {
		error = values_flex_basis(&tokens[0], value);
		return error;
	}

	/* z-index: auto or an integer. */
	if (property == CSS_PROP_Z_INDEX) {
		is_auto = css_ident_equal(&tokens[0], "auto");
		if (is_auto) {
			value->kind = CSS_VALUE_KEYWORD;
			value->keyword = 0;
			return 0;
		}

		/* An integer (a number token without a fraction). */
		if (tokens[0].type != CSS_TOKEN_NUMBER ||
		    !tokens[0].integer)
			return EINVAL;
		value->kind = CSS_VALUE_NUMBER;
		value->number = (float)tokens[0].number;
		return 0;
	}

	/* font-weight: a keyword or a number from 1 to 1000. */
	if (property == CSS_PROP_FONT_WEIGHT) {
		found = values_keyword(values_font_weight, &tokens[0], &keyword);
		if (!found &&
		    tokens[0].type == CSS_TOKEN_NUMBER &&
		    tokens[0].number >= 1 &&
		    tokens[0].number <= 1000) {
			found = 1;
			keyword = (int)tokens[0].number;
		}

		/* Anything else is not a weight. */
		if (!found)
			return EINVAL;

		/* Publish the validated native keyword after its property-specific restrictions. */
		value->kind = CSS_VALUE_KEYWORD;
		value->keyword = keyword;
		return 0;
	}

	/* font-size: a keyword or a length. */
	if (property == CSS_PROP_FONT_SIZE) {
		found = values_keyword(values_font_size, &tokens[0], &keyword);
		if (found) {
			/* A keyword's size at the default size, marked as a keyword so the monospace family can rescale it. */
			value->kind = CSS_VALUE_LENGTH;
			value->number = (float)keyword;
			value->unit = CSS_DUNIT_FONT_KEYWORD;
			return 0;
		}

		/* The relative sizes. */
		found = values_keyword(values_relative_size, &tokens[0], &keyword);
		if (found) {
			value->kind = CSS_VALUE_LENGTH;
			value->number = (float)keyword;
			value->unit = CSS_DUNIT_PERCENT;
			return 0;
		}

		/* Otherwise a length (no keywords). */
		/* Parse the selected component with its established native grammar. */
		error = values_length(&tokens[0], 0, value);
		if (error != 0)
			return error;

		/* Succeeded: the selected native scalar component was parsed. */
		return 0;
	}

	/* line-height: normal, a number or a length. */
	if (property == CSS_PROP_LINE_HEIGHT) {
		if (tokens[0].type == CSS_TOKEN_NUMBER) {
			value->kind = CSS_VALUE_NUMBER;
			value->number = (float)tokens[0].number;
			return 0;
		}

		/* Or a length, or normal. */
		/* Parse the selected component with its established native grammar. */
		error = values_length(&tokens[0], 1, value);
		if (error != 0)
			return error;

		/* Succeeded: the selected native scalar component was parsed. */
		return 0;
	}

	/* Border widths: a keyword or a length. */
	if (property >= CSS_PROP_BORDER_TOP_WIDTH &&
	    property <= CSS_PROP_BORDER_LEFT_WIDTH) {
		found = values_keyword(values_border_width, &tokens[0], &keyword);
		if (found) {
			value->kind = CSS_VALUE_LENGTH;
			value->number = (float)keyword;
			value->unit = CSS_DUNIT_PX;
			return 0;
		}

		/* Or a length. */
		/* Parse the selected component with its established native grammar. */
		error = values_length(&tokens[0], 0, value);
		if (error != 0)
			return error;

		/* Succeeded: the selected native scalar component was parsed. */
		return 0;
	}

	/* Padding cannot be negative; reject the declaration before it replaces an earlier value. */
	error = values_length(&tokens[0], 1, value);
	if (error != 0)
		return error;
	if (property >= CSS_PROP_PADDING_TOP &&
	    property <= CSS_PROP_PADDING_LEFT &&
	    value->kind == CSS_VALUE_LENGTH && value->number < 0)
		return EINVAL;
	/* Succeeded: the property accepted its final native length component. */
	return 0;
}

/* Parses font-family: names (identifiers joined by spaces, or strings) separated by commas. */
static int
values_families(
	struct css_parse *parse,
	const struct css_token *tokens,
	size_t count,
	struct css_value *value)
{
	struct wb_units name;
	size_t index;
	int error;

	/* Reads each family between the commas. */
	value->kind = CSS_VALUE_FAMILIES;
	value->family_count = 0;
	wb_units_init(&name);
	index = 0;
	while (index <= count) {
		/* A comma or the end finishes the name gathered. */
		if (index == count || tokens[index].type == CSS_TOKEN_COMMA) {
			while (name.length > 0 && name.data[name.length - 1U] == ' ')
				name.length--;
			if (name.length != 0 && value->family_count < CSS_DECLARED_FAMILIES) {
				value->families[value->family_count] = vm_atom_from_units(parse->heap, name.data, name.length);
				if (value->families[value->family_count] == NULL) {
					wb_units_release(&name);
					return ENOMEM;
				}

				/* The family is kept. */
				value->family_count++;
			}

			/* The next name starts empty. */
			wb_units_clear(&name);
			index++;
			continue;
		}

		/* Identifiers and strings make the name; whitespace between words is one space. */
		if (tokens[index].type == CSS_TOKEN_IDENT || tokens[index].type == CSS_TOKEN_STRING) {
			error = wb_units_append(&name, tokens[index].text, tokens[index].length);
			if (error != 0) {
				wb_units_release(&name);
				return ENOMEM;
			}
		} else if (tokens[index].type == CSS_TOKEN_WHITESPACE && name.length != 0) {
			error = wb_units_append_code_point(&name, ' ');
			if (error != 0) {
				wb_units_release(&name);
				return ENOMEM;
			}
		} else if (tokens[index].type != CSS_TOKEN_WHITESPACE) {
			wb_units_release(&name);
			return EINVAL;
		}

		/* The next token. */
		index++;
	}

	/* The gathered name is no longer needed. */
	wb_units_release(&name);

	/* A family list needs a family. */
	if (value->family_count == 0)
		return EINVAL;

	/* Succeeded: the families in order. */
	return 0;
}

/* Splits a value into its space-separated components (functions whole); returns how many. */
static size_t
values_components(
	const struct css_token *tokens,
	size_t count,
	size_t *starts,
	size_t *lengths,
	size_t max)
{
	size_t index;
	size_t made;
	size_t start;
	int depth;

	/* Walks the tokens, keeping function arguments with their function. */
	made = 0;
	index = 0;
	while (index < count && made < max) {
		if (tokens[index].type == CSS_TOKEN_WHITESPACE) {
			index++;
			continue;
		}

		/* The component starts here and runs to whitespace outside a function. */
		start = index;
		depth = 0;
		do {
			if (tokens[index].type == CSS_TOKEN_FUNCTION || tokens[index].type == CSS_TOKEN_OPEN_PAREN)
				depth++;
			if (tokens[index].type == CSS_TOKEN_CLOSE_PAREN)
				depth--;
			index++;
		} while (index < count && (depth > 0 || tokens[index].type != CSS_TOKEN_WHITESPACE));
		starts[made] = start;
		lengths[made] = index - start;
		made++;
	}

	/* Reports how many components there are. */
	return made;
}

/* Parses a four-sided shorthand (margin, padding, border-width/style/color). */
static int
values_four(
	struct css_parse *parse,
	int first,
	const struct css_token *tokens,
	size_t count,
	struct css_declaration *out,
	size_t *made)
{
	struct css_value values[4];
	size_t starts[5];
	size_t lengths[5];
	size_t components;
	size_t index;
	int error;

	/* One to four components. */
	components = values_components(tokens, count, starts, lengths, 5);
	if (components == 0 || components > 4)
		return EINVAL;
	for (index = 0; index < components; index++) {
		error = values_single(parse, first, tokens + starts[index], lengths[index], &values[index]);
		if (error != 0)
			return error;
	}

	/* Top, right, bottom, left, the missing ones copied from their opposites. */
	if (components < 2)
		values[1] = values[0];
	if (components < 3)
		values[2] = values[0];
	if (components < 4)
		values[3] = values[1];
	for (index = 0; index < 4; index++)
		values_add(out, made, first + (int)index, &values[index]);

	/* Succeeded: four longhands. */
	return 0;
}

/* Parses a border shorthand for one side, or all four when side is -1. */
static int
values_border(
	struct css_parse *parse,
	int side,
	const struct css_token *tokens,
	size_t count,
	struct css_declaration *out,
	size_t *made)
{
	struct css_value width;
	struct css_value style;
	struct css_value color;
	struct css_value candidate;
	size_t starts[4];
	size_t lengths[4];
	size_t components;
	size_t index;
	int error;
	int first;
	int last;

	/* The defaults: medium, none, currentcolor. */
	memset(&width, 0, sizeof(width));
	width.kind = CSS_VALUE_LENGTH;
	width.number = 3;
	width.unit = CSS_DUNIT_PX;
	memset(&style, 0, sizeof(style));
	style.kind = CSS_VALUE_KEYWORD;
	style.keyword = CSS_BORDER_NONE;
	memset(&color, 0, sizeof(color));
	color.kind = CSS_VALUE_COLOR;
	color.color = CSS_CURRENT_COLOR;

	/* Each component is a width, a style or a color, in any order. */
	components = values_components(tokens, count, starts, lengths, 4);
	if (components == 0 || components > 3)
		return EINVAL;
	for (index = 0; index < components; index++) {
		error = values_single(parse, CSS_PROP_BORDER_TOP_STYLE, tokens + starts[index], lengths[index], &candidate);
		if (error == 0) {
			style = candidate;
			continue;
		}

		/* Or a width. */
		error = values_single(parse, CSS_PROP_BORDER_TOP_WIDTH, tokens + starts[index], lengths[index], &candidate);
		if (error == 0) {
			width = candidate;
			continue;
		}

		/* Or a color. */
		error = values_single(parse, CSS_PROP_BORDER_TOP_COLOR, tokens + starts[index], lengths[index], &candidate);
		if (error == 0) {
			color = candidate;
			continue;
		}

		/* Anything else spoils the shorthand. */
		return EINVAL;
	}

	/* A logical side declares its logical longhands. */
	if (side >= VALUES_LOGICAL_SIDE) {
		values_add(out, made, CSS_PROP_BORDER_INLINE_START_WIDTH + side - VALUES_LOGICAL_SIDE, &width);
		values_add(out, made, CSS_PROP_BORDER_INLINE_START_STYLE + side - VALUES_LOGICAL_SIDE, &style);
		values_add(out, made, CSS_PROP_BORDER_INLINE_START_COLOR + side - VALUES_LOGICAL_SIDE, &color);
		return 0;
	}

	/* Declares the three longhands on the side or sides. */
	first = side;
	last = side;
	if (side < 0) {
		first = CSS_TOP;
		last = CSS_LEFT;
	}

	/* Each side gets the three. */
	for (index = (size_t)first; index <= (size_t)last; index++) {
		values_add(out, made, CSS_PROP_BORDER_TOP_WIDTH + (int)index, &width);
		values_add(out, made, CSS_PROP_BORDER_TOP_STYLE + (int)index, &style);
		values_add(out, made, CSS_PROP_BORDER_TOP_COLOR + (int)index, &color);
	}

	/* Succeeded: the longhands are declared. */
	return 0;
}

/* Adds one longhand declaration to the expansion. */
static void
values_add(
	struct css_declaration *out,
	size_t *made,
	int property,
	const struct css_value *value)
{
	/* Fills the next place: an ordinary declaration, neither custom nor pending. */
	memset(&out[*made], 0, sizeof(out[*made]));
	out[*made].property = property;
	out[*made].important = 0;
	out[*made].value = *value;
	(*made)++;
}

/* Reads a hexadecimal digit, or reports -1. */
static int
values_hex_digit(
	uint16_t unit)
{
	/* The three ranges of hex digits. */
	if (unit >= '0' && unit <= '9')
		return unit - '0';
	if (unit >= 'a' && unit <= 'f')
		return unit - 'a' + 10;
	if (unit >= 'A' && unit <= 'F')
		return unit - 'A' + 10;

	/* Anything else is not a hex digit. */
	return -1;
}

/* Parses the arguments of rgb() or rgba(): three numbers or percentages and an optional alpha. */
static int
values_rgb_function(
	const struct css_token *tokens,
	size_t count,
	uint32_t *color)
{
	double channels[4];
	size_t index;
	int made;
	double value;

	/* Reads up to four numeric arguments, separated by commas, spaces or a slash. */
	made = 0;
	channels[3] = 1.0;
	for (index = 0; index < count && tokens[index].type != CSS_TOKEN_CLOSE_PAREN; index++) {
		if (tokens[index].type == CSS_TOKEN_NUMBER || tokens[index].type == CSS_TOKEN_PERCENTAGE) {
			if (made >= 4)
				return EINVAL;
			value = tokens[index].number;
			if (tokens[index].type == CSS_TOKEN_PERCENTAGE && made == 3) {
				/* An alpha percentage is a fraction of one. */
				value = value / 100.0;
			} else if (tokens[index].type == CSS_TOKEN_PERCENTAGE) {
				/* A channel percentage is a fraction of 255. */
				value = value * 255.0 / 100.0;
			}

			/* The channel is kept. */
			channels[made] = value;
			made++;
			continue;
		}

		/* Separators are skipped. */
		if (tokens[index].type == CSS_TOKEN_WHITESPACE || tokens[index].type == CSS_TOKEN_COMMA)
			continue;
		if (tokens[index].type == CSS_TOKEN_DELIM && tokens[index].delim == '/')
			continue;
		return EINVAL;
	}

	/* Three channels are needed. */
	if (made < 3)
		return EINVAL;

	/* Clamps and packs the channels. */
	for (index = 0; index < 3; index++) {
		if (channels[index] < 0)
			channels[index] = 0;
		if (channels[index] > 255)
			channels[index] = 255;
	}

	/* The alpha between zero and one. */
	if (channels[3] < 0)
		channels[3] = 0;
	if (channels[3] > 1)
		channels[3] = 1;
	*color = (uint32_t)(channels[3] * 255.0 + 0.5) << 24 |
	    (uint32_t)(channels[0] + 0.5) << 16 |
	    (uint32_t)(channels[1] + 0.5) << 8 |
	    (uint32_t)(channels[2] + 0.5);

	/* Succeeded: the color is packed. */
	return 0;
}

/* Parses hsl()/hsla() components and converts their sRGB result to packed RGBA. */
static int
values_hsl_function(
	const struct css_token *tokens,
	size_t count,
	uint32_t *color)
{
	const struct css_token *parts[4];
	double hue;
	double saturation;
	double lightness;
	double alpha;
	double chroma;
	double secondary;
	double offset;
	double red;
	double green;
	double blue;
	size_t index;
	size_t made;
	size_t commas;
	size_t slashes;
	int awaiting;

	/* The legacy form has commas; the modern form uses spaces and a slash. */
	made = 0;
	commas = 0;
	slashes = 0;
	awaiting = 0;
	for (index = 0; index < count && tokens[index].type != CSS_TOKEN_CLOSE_PAREN; index++) {
		if (tokens[index].type == CSS_TOKEN_WHITESPACE)
			continue;
		if (tokens[index].type == CSS_TOKEN_COMMA ||
		    (tokens[index].type == CSS_TOKEN_DELIM && tokens[index].delim == '/')) {
			if (made == 0 || awaiting)
				return EINVAL;
			if (tokens[index].type == CSS_TOKEN_COMMA)
				commas++;
			else
				slashes++;
			awaiting = 1;
			continue;
		}
		if (made >= 4 || (tokens[index].type != CSS_TOKEN_NUMBER &&
		    tokens[index].type != CSS_TOKEN_PERCENTAGE &&
		    tokens[index].type != CSS_TOKEN_DIMENSION))
			return EINVAL;
		parts[made++] = &tokens[index];
		awaiting = 0;
	}
	if (made < 3 || awaiting || (commas != 0 && (slashes != 0 || commas != made - 1)) ||
	    (commas == 0 && slashes != (made == 4 ? 1U : 0U)))
		return EINVAL;

	/* Hue accepts degrees or the three other CSS angle units. */
	if (parts[0]->type == CSS_TOKEN_NUMBER)
		hue = parts[0]->number;
	else if (parts[0]->type == CSS_TOKEN_DIMENSION) {
		hue = parts[0]->number;
		if (css_units_equal_ascii(parts[0]->text, parts[0]->length, "turn"))
			hue *= 360.0;
		else if (css_units_equal_ascii(parts[0]->text, parts[0]->length, "grad"))
			hue *= 0.9;
		else if (css_units_equal_ascii(parts[0]->text, parts[0]->length, "rad"))
			hue *= 180.0 / 3.14159265358979323846;
		else if (!css_units_equal_ascii(parts[0]->text, parts[0]->length, "deg"))
			return EINVAL;
	} else
		return EINVAL;
	if ((parts[1]->type != CSS_TOKEN_PERCENTAGE &&
	    (commas != 0 || parts[1]->type != CSS_TOKEN_NUMBER)) ||
	    (parts[2]->type != CSS_TOKEN_PERCENTAGE &&
	    (commas != 0 || parts[2]->type != CSS_TOKEN_NUMBER)))
		return EINVAL;
	saturation = parts[1]->number / 100.0;
	lightness = parts[2]->number / 100.0;
	alpha = 1.0;
	if (made == 4) {
		if (parts[3]->type != CSS_TOKEN_NUMBER && parts[3]->type != CSS_TOKEN_PERCENTAGE)
			return EINVAL;
		alpha = parts[3]->number;
		if (parts[3]->type == CSS_TOKEN_PERCENTAGE)
			alpha /= 100.0;
	}
	if (!isfinite(hue) || !isfinite(saturation) || !isfinite(lightness) || !isfinite(alpha))
		return EINVAL;
	if (saturation < 0.0)
		saturation = 0.0;
	if (saturation > 1.0)
		saturation = 1.0;
	if (lightness < 0.0)
		lightness = 0.0;
	if (lightness > 1.0)
		lightness = 1.0;
	if (alpha < 0.0)
		alpha = 0.0;
	if (alpha > 1.0)
		alpha = 1.0;
	hue = fmod(hue, 360.0);
	if (hue < 0.0)
		hue += 360.0;
	chroma = (1.0 - fabs(2.0 * lightness - 1.0)) * saturation;
	secondary = chroma * (1.0 - fabs(fmod(hue / 60.0, 2.0) - 1.0));
	offset = lightness - chroma / 2.0;
	red = green = blue = 0.0;
	if (hue < 60.0) {
		red = chroma;
		green = secondary;
	} else if (hue < 120.0) {
		red = secondary;
		green = chroma;
	} else if (hue < 180.0) {
		green = chroma;
		blue = secondary;
	} else if (hue < 240.0) {
		green = secondary;
		blue = chroma;
	} else if (hue < 300.0) {
		red = secondary;
		blue = chroma;
	} else {
		red = chroma;
		blue = secondary;
	}
	*color = (uint32_t)(alpha * 255.0 + 0.5) << 24 |
	    (uint32_t)((red + offset) * 255.0 + 0.5) << 16 |
	    (uint32_t)((green + offset) * 255.0 + 0.5) << 8 |
	    (uint32_t)((blue + offset) * 255.0 + 0.5);
	return 0;
}

/* Declares a CSS-wide keyword for a longhand, or for every longhand of a four-sided shorthand. */
static void
values_wide_keyword(
	int property,
	const struct css_value *value,
	struct css_declaration *out,
	size_t *made)
{
	int first;

	/* A longhand takes it itself. */
	if (property < CSS_PROP_COUNT) {
		values_add(out, made, property, value);
		return;
	}

	/* The four-sided shorthands give it to their four longhands. */
	first = -1;
	switch (property) {
	case SHORT_MARGIN:
		first = CSS_PROP_MARGIN_TOP;
		break;
	case SHORT_MARGIN_INLINE:
		values_add(out, made, CSS_PROP_MARGIN_INLINE_START, value);
		values_add(out, made, CSS_PROP_MARGIN_INLINE_END, value);
		return;
	case SHORT_MARGIN_BLOCK:
		values_add(out, made, CSS_PROP_MARGIN_BLOCK_START, value);
		values_add(out, made, CSS_PROP_MARGIN_BLOCK_END, value);
		return;
	case SHORT_PADDING_INLINE:
		values_add(out, made, CSS_PROP_PADDING_INLINE_START, value);
		values_add(out, made, CSS_PROP_PADDING_INLINE_END, value);
		return;
	case SHORT_PADDING_BLOCK:
		values_add(out, made, CSS_PROP_PADDING_BLOCK_START, value);
		values_add(out, made, CSS_PROP_PADDING_BLOCK_END, value);
		return;
	case SHORT_INSET_INLINE:
		values_add(out, made, CSS_PROP_INSET_INLINE_START, value);
		values_add(out, made, CSS_PROP_INSET_INLINE_END, value);
		return;
	case SHORT_INSET_BLOCK:
		values_add(out, made, CSS_PROP_INSET_BLOCK_START, value);
		values_add(out, made, CSS_PROP_INSET_BLOCK_END, value);
		return;
	case SHORT_BORDER_INLINE_START:
	case SHORT_BORDER_INLINE_END:
	case SHORT_BORDER_BLOCK_START:
	case SHORT_BORDER_BLOCK_END:
		values_add(out, made, CSS_PROP_BORDER_INLINE_START_WIDTH + property - SHORT_BORDER_INLINE_START, value);
		values_add(out, made, CSS_PROP_BORDER_INLINE_START_STYLE + property - SHORT_BORDER_INLINE_START, value);
		values_add(out, made, CSS_PROP_BORDER_INLINE_START_COLOR + property - SHORT_BORDER_INLINE_START, value);
		return;
	case SHORT_PADDING:
		first = CSS_PROP_PADDING_TOP;
		break;
	case SHORT_BORDER_WIDTH:
		first = CSS_PROP_BORDER_TOP_WIDTH;
		break;
	case SHORT_BORDER_STYLE:
		first = CSS_PROP_BORDER_TOP_STYLE;
		break;
	case SHORT_BORDER_COLOR:
		first = CSS_PROP_BORDER_TOP_COLOR;
		break;
	case SHORT_POSITION_TRY:
		values_add(out, made, CSS_PROP_POSITION_TRY_ORDER, value);
		values_add(out, made, CSS_PROP_POSITION_TRY_FALLBACKS, value);
		return;
	case SHORT_TRANSFORM:
		values_add(out, made, CSS_PROP_TRANSFORM_SET, value);
		values_add(out, made, CSS_PROP_TRANSFORM_X, value);
		values_add(out, made, CSS_PROP_TRANSFORM_Y, value);
		values_add(out, made, CSS_PROP_TRANSFORM_SCALE_X, value);
		values_add(out, made, CSS_PROP_TRANSFORM_SCALE_Y, value);
		return;
	case SHORT_ASPECT_RATIO:
		values_add(out, made, CSS_PROP_ASPECT_RATIO, value);
		values_add(out, made, CSS_PROP_ASPECT_AUTO, value);
		return;
	case SHORT_OBJECT_POSITION:
		values_add(out, made, CSS_PROP_OBJECT_POSITION_X, value);
		values_add(out, made, CSS_PROP_OBJECT_POSITION_Y, value);
		return;
	case SHORT_TRANSFORM_ORIGIN:
		values_add(out, made, CSS_PROP_TRANSFORM_ORIGIN_X, value);
		values_add(out, made, CSS_PROP_TRANSFORM_ORIGIN_Y, value);
		return;
	case SHORT_BACKGROUND_POSITION:
		values_add(out, made, CSS_PROP_BACKGROUND_POSITION_X, value);
		values_add(out, made, CSS_PROP_BACKGROUND_POSITION_Y, value);
		return;
	case SHORT_BACKGROUND_SIZE:
		values_add(out, made, CSS_PROP_BACKGROUND_SIZE_WIDTH, value);
		values_add(out, made, CSS_PROP_BACKGROUND_SIZE_HEIGHT, value);
		return;
	case SHORT_BACKGROUND:
		values_add(out, made, CSS_PROP_BACKGROUND_COLOR, value);
		values_add(out, made, CSS_PROP_BACKGROUND_IMAGE, value);
		values_add(out, made, CSS_PROP_BACKGROUND_REPEAT, value);
		values_add(out, made, CSS_PROP_BACKGROUND_POSITION_X, value);
		values_add(out, made, CSS_PROP_BACKGROUND_POSITION_Y, value);
		values_add(out, made, CSS_PROP_BACKGROUND_SIZE_WIDTH, value);
		values_add(out, made, CSS_PROP_BACKGROUND_SIZE_HEIGHT, value);
		return;
	case SHORT_TEXT_DECORATION:
		values_add(out, made, CSS_PROP_TEXT_DECORATION_LINE, value);
		values_add(out, made, CSS_PROP_TEXT_DECORATION_THICKNESS, value);
		values_add(out, made, CSS_PROP_TEXT_DECORATION_STYLE, value);
		values_add(out, made, CSS_PROP_TEXT_DECORATION_COLOR, value);
		return;
	case SHORT_BORDER_RADIUS:
		/* The eight radii. */
		for (first = CSS_PROP_RADIUS_TOP_LEFT_X; first <= CSS_PROP_RADIUS_BOTTOM_LEFT_Y; first++)
			values_add(out, made, first, value);
		return;
	case SHORT_RADIUS_TOP_LEFT:
	case SHORT_RADIUS_TOP_RIGHT:
	case SHORT_RADIUS_BOTTOM_RIGHT:
	case SHORT_RADIUS_BOTTOM_LEFT:
		/* A corner's two radii. */
		first = CSS_PROP_RADIUS_TOP_LEFT_X + 2 * (property - SHORT_RADIUS_TOP_LEFT);
		values_add(out, made, first, value);
		values_add(out, made, first + 1, value);
		return;
	case SHORT_OUTLINE:
		/* The outline's width, style and color. */
		values_add(out, made, CSS_PROP_OUTLINE_WIDTH, value);
		values_add(out, made, CSS_PROP_OUTLINE_STYLE, value);
		values_add(out, made, CSS_PROP_OUTLINE_COLOR, value);
		return;
	case SHORT_GRID_COLUMN:
		/* A grid item's column lines. */
		values_add(out, made, CSS_PROP_GRID_COLUMN_START, value);
		values_add(out, made, CSS_PROP_GRID_COLUMN_END, value);
		return;
	case SHORT_GRID_ROW:
		/* Its row lines. */
		values_add(out, made, CSS_PROP_GRID_ROW_START, value);
		values_add(out, made, CSS_PROP_GRID_ROW_END, value);
		return;
	case SHORT_GRID_AREA:
		/* All four. */
		for (first = CSS_PROP_GRID_COLUMN_START; first <= CSS_PROP_GRID_ROW_END; first++)
			values_add(out, made, first, value);
		return;
	case SHORT_GRID_TEMPLATE:
		/* The explicit grid. */
		values_add(out, made, CSS_PROP_GRID_TEMPLATE_ROWS, value);
		values_add(out, made, CSS_PROP_GRID_TEMPLATE_COLUMNS, value);
		values_add(out, made, CSS_PROP_GRID_TEMPLATE_AREAS, value);
		return;
	case SHORT_GRID:
		/* The explicit grid and the implicit one. */
		values_add(out, made, CSS_PROP_GRID_TEMPLATE_ROWS, value);
		values_add(out, made, CSS_PROP_GRID_TEMPLATE_COLUMNS, value);
		values_add(out, made, CSS_PROP_GRID_TEMPLATE_AREAS, value);
		values_add(out, made, CSS_PROP_GRID_AUTO_FLOW, value);
		values_add(out, made, CSS_PROP_GRID_AUTO_ROWS, value);
		values_add(out, made, CSS_PROP_GRID_AUTO_COLUMNS, value);
		return;
	default:
		break;
	}

	/* The other shorthands are left alone in this pass. */
	if (first < 0)
		return;

	/* Declares the four. */
	values_add(out, made, first, value);
	values_add(out, made, first + 1, value);
	values_add(out, made, first + 2, value);
	values_add(out, made, first + 3, value);
}

/*
 * Parses the background shorthand: its color, image, repeat, position
 * and, after a slash, its size, in any order; what it leaves out takes
 * its initial value (transparent, none, repeat, 0% 0%, auto).
 */
static int
values_background(
	struct css_parse *parse,
	const struct css_token *tokens,
	size_t count,
	struct css_declaration *out,
	size_t *made)
{
	const struct css_token *positions[2];
	const struct css_token *sizes[2];
	const struct css_token *repeats[2];
	struct css_value color;
	struct css_value image;
	struct css_value repeat;
	struct css_value attachment;
	struct css_value x;
	struct css_value y;
	struct css_value width;
	struct css_value height;
	size_t position_count;
	size_t size_count;
	size_t repeat_count;
	size_t index;
	size_t rest;
	int after_slash;
	int attachment_seen;
	int color_seen;
	int image_seen;
	int is_url;
	int named;
	int keyword;
	int found;
	int error;

	/* The initial values. */
	memset(&color, 0, sizeof(color));
	color.kind = CSS_VALUE_COLOR;
	memset(&image, 0, sizeof(image));
	image.kind = CSS_VALUE_KEYWORD;
	memset(&attachment, 0, sizeof(attachment));
	attachment.kind = CSS_VALUE_KEYWORD;
	attachment.keyword = CSS_BACKGROUND_SCROLL;
	position_count = 0;
	size_count = 0;
	repeat_count = 0;
	after_slash = 0;
	attachment_seen = 0;
	color_seen = 0;
	image_seen = 0;

	/* Each component, token by token (a function with its arguments). */
	index = 0;
	while (index < count) {
		/* Whitespace separates components. */
		if (tokens[index].type == CSS_TOKEN_WHITESPACE) {
			index++;
			continue;
		}

		/* A slash: the size follows. */
		if (tokens[index].type == CSS_TOKEN_DELIM && tokens[index].delim == '/') {
			after_slash = 1;
			index++;
			continue;
		}

		/* The tokens of the component: one, or a function to its closing parenthesis. */
		rest = values_function_length(tokens + index, count - index);

		/* An image: url(...) or none. */
		is_url = 0;
		named = css_ident_equal(&tokens[index], "url");
		if (tokens[index].type == CSS_TOKEN_URL)
			is_url = 1;
		if (tokens[index].type == CSS_TOKEN_FUNCTION && named)
			is_url = 1;
		if (is_url) {
			if (image_seen)
				return EINVAL;
			error = values_url(parse, tokens + index, rest, &image);
			if (error != 0)
				return error;
			image_seen = 1;
			index += rest;
			continue;
		}

		/* none is no image. */
		named = css_ident_equal(&tokens[index], "none");
		if (named) {
			if (image_seen)
				return EINVAL;
			image.kind = CSS_VALUE_KEYWORD;
			image.keyword = 0;
			image_seen = 1;
			index += rest;
			continue;
		}

		/* The attachment controls whether positioning uses the box or the viewport. */
		named = values_keyword(values_attachment, &tokens[index], &keyword);
		if (named) {
			if (attachment_seen)
				return EINVAL;
			attachment_seen = 1;
			attachment.keyword = keyword;
			index += rest;
			continue;
		}

		/* A repeat keyword. */
		found = values_keyword(values_repeat, &tokens[index], &keyword);
		if (found && repeat_count < 2) {
			repeats[repeat_count] = &tokens[index];
			repeat_count++;
			index += rest;
			continue;
		}

		/* After the slash, a part of the size. */
		if (after_slash && size_count < 2) {
			sizes[size_count] = &tokens[index];
			size_count++;
			index += rest;
			continue;
		}

		/* A position keyword or a length is a part of the position. */
		found = values_keyword(values_position_keywords, &tokens[index], &keyword);
		if (tokens[index].type == CSS_TOKEN_NUMBER || tokens[index].type == CSS_TOKEN_PERCENTAGE || tokens[index].type == CSS_TOKEN_DIMENSION)
			found = 1;
		if (found && position_count < 2) {
			positions[position_count] = &tokens[index];
			position_count++;
			index += rest;
			continue;
		}

		/* Anything else must be the color. */
		if (color_seen)
			return EINVAL;
		error = css_parse_color(&tokens[index], rest, &color.color);
		if (error != 0)
			return EINVAL;
		color_seen = 1;
		index += rest;
	}

	/* The repeat, the position and the size from their parts. */
	error = values_repeat_parts(repeats, repeat_count, &repeat);
	if (error != 0)
		return error;
	error = values_position_parts(positions, position_count, &x, &y);
	if (error != 0)
		return error;
	error = values_size_parts(sizes, size_count, &width, &height);
	if (error != 0)
		return error;

	/* Declares every longhand of the shorthand. */
	values_add(out, made, CSS_PROP_BACKGROUND_COLOR, &color);
	values_add(out, made, CSS_PROP_BACKGROUND_IMAGE, &image);
	values_add(out, made, CSS_PROP_BACKGROUND_REPEAT, &repeat);
	values_add(out, made, CSS_PROP_BACKGROUND_ATTACHMENT, &attachment);
	values_add(out, made, CSS_PROP_BACKGROUND_POSITION_X, &x);
	values_add(out, made, CSS_PROP_BACKGROUND_POSITION_Y, &y);
	values_add(out, made, CSS_PROP_BACKGROUND_SIZE_WIDTH, &width);
	values_add(out, made, CSS_PROP_BACKGROUND_SIZE_HEIGHT, &height);

	/* Succeeded: the longhands are declared. */
	return 0;
}

/* Measures a component's tokens: one, or a function or a parenthesis with its arguments. */
static size_t
values_function_length(
	const struct css_token *tokens,
	size_t count)
{
	size_t index;
	int depth;

	/* A plain token is one. */
	if (tokens[0].type != CSS_TOKEN_FUNCTION && tokens[0].type != CSS_TOKEN_OPEN_PAREN)
		return 1;

	/* To the parenthesis that closes it (or the end). */
	depth = 0;
	for (index = 0; index < count; index++) {
		if (tokens[index].type == CSS_TOKEN_FUNCTION || tokens[index].type == CSS_TOKEN_OPEN_PAREN)
			depth++;
		if (tokens[index].type == CSS_TOKEN_CLOSE_PAREN)
			depth--;
		if (depth == 0)
			return index + 1U;
	}

	/* An unclosed function runs to the end. */
	return count;
}

/*
 * Parses an image: url(...) (a URL token, or the url function with a
 * string) into an atom of its text, or none.
 */
static int
values_url(
	struct css_parse *parse,
	const struct css_token *tokens,
	size_t count,
	struct css_value *value)
{
	size_t index;
	int named;

	/* none. */
	memset(value, 0, sizeof(*value));
	named = css_ident_equal(&tokens[0], "none");
	if (count == 1 && named) {
		value->kind = CSS_VALUE_KEYWORD;
		return 0;
	}

	/* A URL token carries its text. */
	if (count == 1 && tokens[0].type == CSS_TOKEN_URL) {
		value->url = vm_atom_from_units(parse->heap, tokens[0].text, tokens[0].length);
		if (value->url == NULL)
			return ENOMEM;
		value->kind = CSS_VALUE_URL;
		return 0;
	}

	/* url( "string" ): the function, whitespace, the string, whitespace, the parenthesis. */
	named = css_ident_equal(&tokens[0], "url");
	if (tokens[0].type != CSS_TOKEN_FUNCTION || !named)
		return EINVAL;
	index = 1;
	while (index < count && tokens[index].type == CSS_TOKEN_WHITESPACE)
		index++;
	if (index >= count || tokens[index].type != CSS_TOKEN_STRING)
		return EINVAL;

	/* The string's text. */
	value->url = vm_atom_from_units(parse->heap, tokens[index].text, tokens[index].length);
	if (value->url == NULL)
		return ENOMEM;

	/* Succeeded: the URL. */
	value->kind = CSS_VALUE_URL;
	return 0;
}

/*
 * Makes background-repeat from its keywords: one (repeat, repeat-x,
 * repeat-y, no-repeat; space and round repeat), or one for each axis;
 * none is repeat.
 */
static int
values_repeat_parts(
	const struct css_token *const *parts,
	size_t count,
	struct css_value *value)
{
	int second;
	int across;
	int down;
	int found;

	/* The initial value. */
	memset(value, 0, sizeof(*value));
	value->kind = CSS_VALUE_KEYWORD;
	value->keyword = CSS_REPEAT_BOTH;
	if (count == 0)
		return 0;

	/* One keyword names both axes. */
	found = values_keyword(values_repeat, parts[0], &value->keyword);
	if (!found)
		return EINVAL;
	if (count == 1)
		return 0;

	/* Two keywords: each axis repeats or not (repeat-x and repeat-y name both axes, and cannot be one of two). */
	found = values_keyword(values_repeat, parts[1], &second);
	if (!found)
		return EINVAL;
	if (value->keyword == CSS_REPEAT_X || value->keyword == CSS_REPEAT_Y)
		return EINVAL;
	if (second == CSS_REPEAT_X || second == CSS_REPEAT_Y)
		return EINVAL;

	/* The axes that do not repeat. */
	across = 0;
	if (value->keyword == CSS_REPEAT_NONE)
		across = 1;
	down = 0;
	if (second == CSS_REPEAT_NONE)
		down = 1;

	/* Decide by the axes that do not repeat. */
	if (!across && !down) {
		value->keyword = CSS_REPEAT_BOTH;
	} else if (!across) {
		value->keyword = CSS_REPEAT_X;
	} else if (!down) {
		value->keyword = CSS_REPEAT_Y;
	} else {
		value->keyword = CSS_REPEAT_NONE;
	}

	/* Succeeded: the repeat. */
	return 0;
}

/*
 * Makes background-position's two axes from one or two parts: keywords
 * (left, center, right, top, bottom, as percentages) or lengths.  One part
 * is centred on the other axis; top and bottom name the vertical axis
 * wherever they are.
 */
static int
values_position_parts(
	const struct css_token *const *parts,
	size_t count,
	struct css_value *x,
	struct css_value *y)
{
	struct css_value first;
	struct css_value second;
	int first_vertical;
	int second_horizontal;
	int axis;
	int error;

	/* The initial value, 0% 0%. */
	memset(x, 0, sizeof(*x));
	x->kind = CSS_VALUE_LENGTH;
	x->unit = CSS_DUNIT_PERCENT;
	*y = *x;
	if (count == 0)
		return 0;

	/* The first part. */
	error = values_position_part(parts[0], &first);
	if (error != 0)
		return error;
	axis = values_position_axis(parts[0]);
	first_vertical = 0;
	if (axis == 2)
		first_vertical = 1;

	/* One part: it and the centre of the other axis. */
	if (count == 1) {
		second.kind = CSS_VALUE_LENGTH;
		second.number = 50.0f;
		second.unit = CSS_DUNIT_PERCENT;
		*x = first;
		*y = second;
		if (first_vertical) {
			*x = second;
			*y = first;
		}

		/* Succeeded: one axis given, the other centred. */
		return 0;
	}

	/* Two parts, horizontal first unless the keywords say otherwise. */
	error = values_position_part(parts[1], &second);
	if (error != 0)
		return error;
	axis = values_position_axis(parts[1]);
	second_horizontal = 0;
	if (axis == 1)
		second_horizontal = 1;
	*x = first;
	*y = second;
	if (first_vertical || second_horizontal) {
		*x = second;
		*y = first;
	}

	/* Succeeded: both axes. */
	return 0;
}

/* Tells which axis a part of a position names: 1 for left and right, 2 for top and bottom, 0 for any other. */
static int
values_position_axis(
	const struct css_token *token)
{
	int named;

	/* left and right are horizontal. */
	named = css_ident_equal(token, "left");
	if (named)
		return 1;
	named = css_ident_equal(token, "right");
	if (named)
		return 1;

	/* top and bottom are vertical. */
	named = css_ident_equal(token, "top");
	if (named)
		return 2;
	named = css_ident_equal(token, "bottom");
	if (named)
		return 2;

	/* center and lengths name neither. */
	return 0;
}

/* Reads one part of a position: a keyword as a percentage, or a length. */
static int
values_position_part(
	const struct css_token *token,
	struct css_value *value)
{
	int keyword;
	int found;
	int error;

	/* A keyword is a percentage of the room. */
	memset(value, 0, sizeof(*value));
	found = values_keyword(values_position_keywords, token, &keyword);
	if (found) {
		value->kind = CSS_VALUE_LENGTH;
		value->number = (float)keyword;
		value->unit = CSS_DUNIT_PERCENT;
		return 0;
	}

	/* A length or a percentage. */
	error = values_length(token, 0, value);
	if (error != 0)
		return error;

	/* Succeeded: the length. */
	return 0;
}

/*
 * Makes background-size's width and height from one or two parts:
 * contain or cover, or a width (and a height; auto when there is none),
 * each a length, a percentage or auto.
 */
static int
values_size_parts(
	const struct css_token *const *parts,
	size_t count,
	struct css_value *width,
	struct css_value *height)
{
	int named;
	int error;

	/* The initial value, auto auto. */
	memset(width, 0, sizeof(*width));
	width->kind = CSS_VALUE_KEYWORD;
	width->keyword = CSS_UNIT_AUTO;
	*height = *width;
	if (count == 0)
		return 0;

	/* contain and cover size both sides. */
	named = css_ident_equal(parts[0], "contain");
	if (named)
		width->keyword = CSS_BACKGROUND_SIZE_CONTAIN;
	named = css_ident_equal(parts[0], "cover");
	if (named)
		width->keyword = CSS_BACKGROUND_SIZE_COVER;
	if (width->keyword != CSS_UNIT_AUTO && count != 1)
		return EINVAL;
	if (width->keyword != CSS_UNIT_AUTO)
		return 0;

	/* The width: a length, a percentage or auto. */
	error = values_length(parts[0], 1, width);
	if (error != 0)
		return error;
	if (count == 1)
		return 0;

	/* The height likewise. */
	error = values_length(parts[1], 1, height);
	if (error != 0)
		return error;

	/* Succeeded: both sides. */
	return 0;
}

/* Parses background-position into its two longhands. */
static int
values_background_position(
	const struct css_token *tokens,
	size_t count,
	struct css_declaration *out,
	size_t *made)
{
	const struct css_token *parts[2];
	struct css_value x;
	struct css_value y;
	size_t found;
	size_t index;
	int error;

	/* Its parts: at most two tokens. */
	found = 0;
	for (index = 0; index < count; index++) {
		if (tokens[index].type == CSS_TOKEN_WHITESPACE)
			continue;
		if (found == 2)
			return EINVAL;
		parts[found] = &tokens[index];
		found++;
	}

	/* The two axes. */
	error = values_position_parts(parts, found, &x, &y);
	if (error != 0)
		return error;
	values_add(out, made, CSS_PROP_BACKGROUND_POSITION_X, &x);
	values_add(out, made, CSS_PROP_BACKGROUND_POSITION_Y, &y);

	/* Succeeded: both declared. */
	return 0;
}

/* Parses background-size into its two longhands. */
static int
values_background_size(
	const struct css_token *tokens,
	size_t count,
	struct css_declaration *out,
	size_t *made)
{
	const struct css_token *parts[2];
	struct css_value width;
	struct css_value height;
	size_t found;
	size_t index;
	int error;

	/* Its parts: at most two tokens. */
	found = 0;
	for (index = 0; index < count; index++) {
		if (tokens[index].type == CSS_TOKEN_WHITESPACE)
			continue;
		if (found == 2)
			return EINVAL;
		parts[found] = &tokens[index];
		found++;
	}

	/* The two sides. */
	error = values_size_parts(parts, found, &width, &height);
	if (error != 0)
		return error;
	values_add(out, made, CSS_PROP_BACKGROUND_SIZE_WIDTH, &width);
	values_add(out, made, CSS_PROP_BACKGROUND_SIZE_HEIGHT, &height);

	/* Succeeded: both declared. */
	return 0;
}

/* Tells whether a longhand takes a length, and so may take a calculation. */
static int
values_takes_length(
	int property)
{
	/* The sizes, the four-sided lengths, the offsets and the font's lengths. */
	switch (property) {
	case CSS_PROP_RADIUS_TOP_LEFT_X:
	case CSS_PROP_RADIUS_TOP_LEFT_Y:
	case CSS_PROP_RADIUS_TOP_RIGHT_X:
	case CSS_PROP_RADIUS_TOP_RIGHT_Y:
	case CSS_PROP_RADIUS_BOTTOM_RIGHT_X:
	case CSS_PROP_RADIUS_BOTTOM_RIGHT_Y:
	case CSS_PROP_RADIUS_BOTTOM_LEFT_X:
	case CSS_PROP_RADIUS_BOTTOM_LEFT_Y:
	case CSS_PROP_WIDTH:
	case CSS_PROP_HEIGHT:
	case CSS_PROP_MIN_WIDTH:
	case CSS_PROP_MAX_WIDTH:
	case CSS_PROP_MIN_HEIGHT:
	case CSS_PROP_MAX_HEIGHT:
	case CSS_PROP_MARGIN_TOP:
	case CSS_PROP_MARGIN_RIGHT:
	case CSS_PROP_MARGIN_BOTTOM:
	case CSS_PROP_MARGIN_LEFT:
	case CSS_PROP_MARGIN_INLINE_START:
	case CSS_PROP_MARGIN_INLINE_END:
	case CSS_PROP_MARGIN_BLOCK_START:
	case CSS_PROP_MARGIN_BLOCK_END:
	case CSS_PROP_PADDING_TOP:
	case CSS_PROP_PADDING_RIGHT:
	case CSS_PROP_PADDING_BOTTOM:
	case CSS_PROP_PADDING_LEFT:
	case CSS_PROP_BORDER_TOP_WIDTH:
	case CSS_PROP_BORDER_RIGHT_WIDTH:
	case CSS_PROP_BORDER_BOTTOM_WIDTH:
	case CSS_PROP_BORDER_LEFT_WIDTH:
	case CSS_PROP_TOP:
	case CSS_PROP_RIGHT:
	case CSS_PROP_BOTTOM:
	case CSS_PROP_LEFT:
	case CSS_PROP_FONT_SIZE:
	case CSS_PROP_LINE_HEIGHT:
	case CSS_PROP_TEXT_INDENT:
	case CSS_PROP_ROW_GAP:
	case CSS_PROP_COLUMN_GAP:
	case CSS_PROP_FLEX_BASIS:
	case CSS_PROP_BACKGROUND_SIZE_WIDTH:
	case CSS_PROP_BACKGROUND_SIZE_HEIGHT:
		return 1;
	default:
		return 0;
	}
}

/*
 * Parses calc(), min(), max() or clamp() (the whole value) into a
 * calculation in the parse's arena: a length whose unit is
 * CSS_DUNIT_CALC, or for line-height a plain number.
 */
static int
values_calc(
	struct css_parse *parse,
	int property,
	const struct css_token *tokens,
	size_t count,
	struct css_value *value)
{
	struct css_calc calc;
	struct css_calc *kept;
	size_t inner;
	size_t used;
	size_t after;
	int operation;
	int same;
	int error;

	/* The function's name picks what it does. */
	operation = -1;
	same = css_ident_equal(&tokens[0], "calc");
	if (!same)
		same = css_ident_equal(&tokens[0], "-webkit-calc");
	if (same)
		operation = CSS_CALC_SUM;
	same = css_ident_equal(&tokens[0], "min");
	if (same)
		operation = CSS_CALC_MIN;
	same = css_ident_equal(&tokens[0], "max");
	if (same)
		operation = CSS_CALC_MAX;
	same = css_ident_equal(&tokens[0], "clamp");
	if (same)
		operation = CSS_CALC_CLAMP;
	if (operation < 0 || count < 2 || tokens[count - 1U].type != CSS_TOKEN_CLOSE_PAREN)
		return EINVAL;

	/* calc() around one min(), max() or clamp() is that function. */
	inner = values_skip_space(tokens, count - 1U, 1);
	if (operation == CSS_CALC_SUM && inner < count - 1U && tokens[inner].type == CSS_TOKEN_FUNCTION) {
		used = values_function_length(tokens + inner, count - 1U - inner);
		after = values_skip_space(tokens, count - 1U, inner + used);
		if (after == count - 1U) {
			error = values_calc(parse, property, tokens + inner, used, value);
			if (error == 0)
				return 0;
		}
	}

	/* The arguments, as sums. */
	memset(&calc, 0, sizeof(calc));
	error = values_calc_arguments(parse->arena, tokens + 1, count - 2U, operation, &calc);
	if (error != 0)
		return error;

	/* A calculation of plain numbers is a number: line-height's, or zero for a length. */
	if (calc.count == 0) {
		if (property == CSS_PROP_LINE_HEIGHT) {
			value->kind = CSS_VALUE_NUMBER;
			value->number = calc.sums[0].px;
			return 0;
		}

		/* Only zero is a length without a unit. */
		if (calc.sums[0].px != 0)
			return EINVAL;
		value->kind = CSS_VALUE_LENGTH;
		value->number = 0;
		value->unit = CSS_DUNIT_PX;
		return 0;
	}

	/* The calculation is kept in the arena. */
	kept = wb_arena_alloc(parse->arena, sizeof(*kept));
	if (kept == NULL)
		return ENOMEM;
	*kept = calc;

	/* Succeeded: a calculated length. */
	value->kind = CSS_VALUE_LENGTH;
	value->unit = CSS_DUNIT_CALC;
	value->number = 0;
	value->calc = kept;
	return 0;
}

/*
 * Reads a calculation's arguments (the tokens inside its parentheses):
 * one sum for calc(), one to three separated by commas for min() and
 * max(), three for clamp().  A calculation of plain numbers leaves count
 * zero with the number in the first sum's px.
 */
static int
values_calc_arguments(
	struct wb_arena *arena,
	const struct css_token *tokens,
	size_t count,
	int operation,
	struct css_calc *calc)
{
	struct values_term term;
	size_t index;
	size_t made;
	int numbers;
	int error;

	/* Each argument in turn. */
	calc->operation = operation;
	index = 0;
	made = 0;
	numbers = 0;
	for (;;) {
		/* Too many arguments spoil the calculation. */
		if (made >= CSS_CALC_ARGUMENTS)
			return EINVAL;

		/* The argument's sum. */
		error = values_calc_sum(arena, tokens, count, &index, &term);
		if (error != 0)
			return error;

		/* A plain number, or a sum of lengths. */
		if (term.is_number) {
			numbers++;
			memset(&calc->sums[made], 0, sizeof(calc->sums[made]));
			calc->sums[made].px = term.number;
		} else {
			calc->sums[made] = term.sum;
		}

		/* One more argument. */
		made++;

		/* A comma before the next argument, or the end. */
		index = values_skip_space(tokens, count, index);
		if (index >= count)
			break;
		if (tokens[index].type != CSS_TOKEN_COMMA || operation == CSS_CALC_SUM)
			return EINVAL;
		index++;
	}

	/* clamp() takes three; numbers and lengths are not mixed. */
	if (operation == CSS_CALC_CLAMP && made != 3)
		return EINVAL;
	if (numbers != 0 && numbers != (int)made)
		return EINVAL;

	/* A calculation of numbers alone is reported as its number. */
	calc->count = made;
	if (numbers != 0) {
		if (made != 1)
			return EINVAL;
		calc->count = 0;
	}

	/* Succeeded: the arguments are read. */
	return 0;
}

/* Reads a sum: products joined by + and -. */
static int
values_calc_sum(
	struct wb_arena *arena,
	const struct css_token *tokens,
	size_t count,
	size_t *index,
	struct values_term *term)
{
	struct values_term right;
	float sign;
	int error;

	/* The first product. */
	error = values_calc_product(arena, tokens, count, index, term);
	if (error != 0)
		return error;

	/* Then + or - and another product, while they follow. */
	for (;;) {
		*index = values_skip_space(tokens, count, *index);
		if (*index >= count || tokens[*index].type != CSS_TOKEN_DELIM)
			break;
		if (tokens[*index].delim != '+' && tokens[*index].delim != '-')
			break;

		/* The operator's sign, then the product after it. */
		sign = 1.0f;
		if (tokens[*index].delim == '-')
			sign = -1.0f;
		(*index)++;
		error = values_calc_product(arena, tokens, count, index, &right);
		if (error != 0)
			return error;

		/* Numbers add to numbers. */
		if (term->is_number && right.is_number) {
			term->number += sign * right.number;
			continue;
		}

		/* A zero number counts as a length; any other number with a length spoils the sum. */
		if (term->is_number) {
			if (term->number != 0)
				return EINVAL;
			memset(&term->sum, 0, sizeof(term->sum));
			term->is_number = 0;
		}

		/* The same for the right side. */
		if (right.is_number) {
			if (right.number != 0)
				return EINVAL;
			continue;
		}

		/* Two lengths (with one min(), max() or clamp() between them at most). */
		error = values_calc_add(&term->sum, &right.sum, sign);
		if (error != 0)
			return error;
	}

	/* Succeeded: the sum is read. */
	return 0;
}

/* Reads a product: values joined by * and /, one side of each a number. */
static int
values_calc_product(
	struct wb_arena *arena,
	const struct css_token *tokens,
	size_t count,
	size_t *index,
	struct values_term *term)
{
	struct values_term right;
	int divide;
	int error;

	/* The first value. */
	error = values_calc_value(arena, tokens, count, index, term);
	if (error != 0)
		return error;

	/* Then * or / and another value, while they follow. */
	for (;;) {
		*index = values_skip_space(tokens, count, *index);
		if (*index >= count || tokens[*index].type != CSS_TOKEN_DELIM)
			break;
		if (tokens[*index].delim != '*' && tokens[*index].delim != '/')
			break;

		/* The operator, then the value after it. */
		divide = 0;
		if (tokens[*index].delim == '/')
			divide = 1;
		(*index)++;
		error = values_calc_value(arena, tokens, count, index, &right);
		if (error != 0)
			return error;

		/* Division is by a number that is not zero. */
		if (divide) {
			if (!right.is_number || right.number == 0)
				return EINVAL;
			if (term->is_number) {
				term->number /= right.number;
			} else {
				values_calc_scale(&term->sum, 1.0f / right.number);
			}

			/* The quotient is the term now. */
			continue;
		}

		/* Multiplication has a number on one side at least. */
		if (term->is_number && right.is_number) {
			term->number *= right.number;
		} else if (term->is_number) {
			values_calc_scale(&right.sum, term->number);
			*term = right;
		} else if (right.is_number) {
			values_calc_scale(&term->sum, right.number);
		} else {
			return EINVAL;
		}
	}

	/* Succeeded: the product is read. */
	return 0;
}

/* Reads one value: a number, a percentage, a length, or a sum in parentheses or calc(). */
static int
values_calc_value(
	struct wb_arena *arena,
	const struct css_token *tokens,
	size_t count,
	size_t *index,
	struct values_term *term)
{
	const struct css_token *token;
	struct css_calc *nested;
	size_t used;
	int operation;
	int is_calc;
	int error;

	/* The token past whitespace. */
	memset(term, 0, sizeof(*term));
	*index = values_skip_space(tokens, count, *index);
	if (*index >= count)
		return EINVAL;
	token = &tokens[*index];

	/* A plain number. */
	if (token->type == CSS_TOKEN_NUMBER) {
		term->is_number = 1;
		term->number = (float)token->number;
		(*index)++;

		/* Succeeded: the atomic calculation term is read. */
		return 0;
	}

	/* A percentage. */
	if (token->type == CSS_TOKEN_PERCENTAGE) {
		term->sum.percent = (float)token->number;
		term->sum.has_percent = 1;
		(*index)++;

		/* Succeeded: the atomic calculation term is read. */
		return 0;
	}

	/* A length. */
	if (token->type == CSS_TOKEN_DIMENSION) {
		error = values_calc_unit(token, &term->sum);
		if (error != 0)
			return error;
		(*index)++;

		/* Succeeded: the atomic calculation term is read. */
		return 0;
	}

	/* A min(), max() or clamp() inside the sum: its arguments, kept in the arena, times one. */
	operation = -1;
	if (token->type == CSS_TOKEN_FUNCTION) {
		is_calc = css_ident_equal(token, "min");
		if (is_calc)
			operation = CSS_CALC_MIN;

		/* Recognizes a maximum expression independently of the preceding name. */
		is_calc = css_ident_equal(token, "max");
		if (is_calc)
			operation = CSS_CALC_MAX;

		/* Recognizes a three-argument range expression. */
		is_calc = css_ident_equal(token, "clamp");
		if (is_calc)
			operation = CSS_CALC_CLAMP;
	}

	/* The function's arguments, as a calculation of its own. */
	if (operation >= 0) {
		used = values_function_length(tokens + *index, count - *index);
		if (used < 2U || tokens[*index + used - 1U].type != CSS_TOKEN_CLOSE_PAREN)
			return EINVAL;

		/* Allocates the nonlinear child in the same parse lifetime. */
		nested = wb_arena_zalloc(arena, sizeof(*nested));
		if (nested == NULL)
			return ENOMEM;

		/* Parses the complete child before publishing its arena pointer. */
		error = values_calc_arguments(arena, tokens + *index + 1U, used - 2U, operation, nested);
		if (error != 0)
			return error;

		/* A nested child must carry a dimensional expression. */
		if (nested->count == 0)
			return EINVAL;

		/* Retains the validated child with its initial multiplication factor. */
		term->sum.nested = nested;
		term->sum.nested_factor = 1.0f;
		*index += used;

		/* Succeeded: the nonlinear calculation term is retained. */
		return 0;
	}

	/* A sum in parentheses, or in a nested calc(). */
	if (token->type == CSS_TOKEN_FUNCTION) {
		is_calc = css_ident_equal(token, "calc");
		if (!is_calc)
			return EINVAL;
	} else if (token->type != CSS_TOKEN_OPEN_PAREN) {
		return EINVAL;
	}

	/* The sum inside, then the closing parenthesis. */
	(*index)++;
	error = values_calc_sum(arena, tokens, count, index, term);
	if (error != 0)
		return error;

	/* Consumes the closing delimiter after a complete parenthesized sum. */
	*index = values_skip_space(tokens, count, *index);
	if (*index >= count || tokens[*index].type != CSS_TOKEN_CLOSE_PAREN)
		return EINVAL;
	(*index)++;

	/* Succeeded: the value is read. */
	return 0;
}

/* Adds a dimension to a sum in the field of its unit (the absolute units as pixels). */
static int
values_calc_unit(
	const struct css_token *token,
	struct css_calc_sum *sum)
{
	float number;
	int unit;
	int found;

	/* The unit must be a length unit. */
	found = values_unit(token, &unit);
	if (!found)
		return EINVAL;
	number = (float)token->number;

	/* Its field. */
	switch (unit) {
	case CSS_DUNIT_PX:
		sum->px += number;
		break;
	case CSS_DUNIT_PT:
		sum->px += number * 96.0f / 72.0f;
		break;
	case CSS_DUNIT_PC:
		sum->px += number * 16.0f;
		break;
	case CSS_DUNIT_IN:
		sum->px += number * 96.0f;
		break;
	case CSS_DUNIT_CM:
		sum->px += number * 96.0f / 2.54f;
		break;
	case CSS_DUNIT_MM:
		sum->px += number * 96.0f / 25.4f;
		break;
	case CSS_DUNIT_EM:
		sum->em += number;
		break;
	case CSS_DUNIT_EX:
		sum->ex += number;
		break;
	case CSS_DUNIT_REM:
		sum->rem += number;
		break;
	case CSS_DUNIT_VW:
		sum->vw += number;
		break;
	case CSS_DUNIT_VH:
		sum->vh += number;
		break;
	case CSS_DUNIT_VMIN:
		sum->vmin += number;
		break;
	case CSS_DUNIT_VMAX:
		sum->vmax += number;
		break;
	case CSS_DUNIT_CQW:
		sum->cqw += number;
		break;
	case CSS_DUNIT_CQH:
		sum->cqh += number;
		break;
	default:
		return EINVAL;
	}

	/* Succeeded: the length is in the sum. */
	return 0;
}

/* Multiplies every field of a sum by a factor. */
static void
values_calc_scale(
	struct css_calc_sum *sum,
	float factor)
{
	/* Each kind of length. */
	sum->px *= factor;
	sum->percent *= factor;
	sum->em *= factor;
	sum->ex *= factor;
	sum->rem *= factor;
	sum->vw *= factor;
	sum->vh *= factor;
	sum->vmin *= factor;
	sum->vmax *= factor;
	sum->cqw *= factor;
	sum->cqh *= factor;

	/* And the function's factor. */
	sum->nested_factor *= factor;
}

/*
 * Adds (sign 1) or subtracts (sign -1) one sum to or from another; two
 * sums that each hold a min(), max() or clamp() are not added in this
 * pass (EINVAL).
 */
static int
values_calc_add(
	struct css_calc_sum *sum,
	const struct css_calc_sum *other,
	float sign)
{
	/* The other's function, when it has one, becomes the sum's. */
	if (other->nested != NULL) {
		/* This representation owns at most one nested nonlinear expression. */
		if (sum->nested != NULL)
			return EINVAL;

		/* Retains the single arena child and applies the addition sign to its factor. */
		sum->nested = other->nested;
		sum->nested_factor = sign * other->nested_factor;
	}

	/* Each kind of length. */
	sum->px += sign * other->px;
	sum->percent += sign * other->percent;
	sum->em += sign * other->em;
	sum->ex += sign * other->ex;
	sum->rem += sign * other->rem;
	sum->vw += sign * other->vw;
	sum->vh += sign * other->vh;
	sum->vmin += sign * other->vmin;
	sum->vmax += sign * other->vmax;
	sum->cqw += sign * other->cqw;
	sum->cqh += sign * other->cqh;

	/* Zero coefficients do not erase a percentage's contribution to the linear type. */
	if (other->has_percent)
		sum->has_percent = 1;

	/* Succeeded: the sums are added. */
	return 0;
}

/* Skips whitespace tokens from index; returns the first other index (or count). */
static size_t
values_skip_space(
	const struct css_token *tokens,
	size_t count,
	size_t index)
{
	/* Moves past the whitespace. */
	while (index < count && tokens[index].type == CSS_TOKEN_WHITESPACE)
		index++;

	/* Reports where the rest starts. */
	return index;
}

/* Parses a two-sided logical shorthand (margin-inline and the like): one value for both sides, or one each. */
static int
values_pair(
	struct css_parse *parse,
	int first,
	int second,
	const struct css_token *tokens,
	size_t count,
	struct css_declaration *out,
	size_t *made)
{
	struct css_value values[2];
	size_t starts[3];
	size_t lengths[3];
	size_t components;
	size_t index;
	int error;

	/* One or two components. */
	components = values_components(tokens, count, starts, lengths, 3);
	if (components == 0 || components > 2)
		return EINVAL;
	for (index = 0; index < components; index++) {
		error = values_single(parse, first, tokens + starts[index], lengths[index], &values[index]);
		if (error != 0)
			return error;
	}

	/* The second side copies the first when it is missing. */
	if (components < 2)
		values[1] = values[0];
	values_add(out, made, first, &values[0]);
	values_add(out, made, second, &values[1]);

	/* Succeeded: two longhands. */
	return 0;
}

/*
 * Parses content: none or normal (no box), or a list of strings and
 * attr(name) items, kept in the parse's arena (counters, quotes and images
 * add nothing in this pass; the alternative text after a slash is not
 * drawn).
 */
static int
values_content(
	struct css_parse *parse,
	const struct css_token *tokens,
	size_t count,
	struct css_value *value)
{
	struct css_content_item items[VALUES_CONTENT_ITEMS];
	struct css_content_item *kept_items;
	struct css_content *kept;
	size_t made;
	size_t index;
	size_t name;
	size_t used;
	int is_word;
	int is_attribute;

	/* none and normal. */
	if (count == 1 && tokens[0].type == CSS_TOKEN_IDENT) {
		is_word = css_ident_equal(&tokens[0], "none");
		if (!is_word)
			is_word = css_ident_equal(&tokens[0], "normal");
		if (!is_word)
			return EINVAL;
		value->kind = CSS_VALUE_KEYWORD;
		value->keyword = 0;
		return 0;
	}

	/* The strings and attr() items in order (up to the room there is). */
	made = 0;
	index = 0;
	while (index < count && made < VALUES_CONTENT_ITEMS) {
		if (tokens[index].type == CSS_TOKEN_DELIM && tokens[index].delim == '/')
			break;

		/* A string. */
		if (tokens[index].type == CSS_TOKEN_STRING) {
			items[made].is_attribute = 0;
			items[made].text = vm_atom_from_units(parse->heap, tokens[index].text, tokens[index].length);
			if (items[made].text == NULL)
				return ENOMEM;
			made++;
		}

		/* attr(name). */
		is_attribute = 0;
		if (tokens[index].type == CSS_TOKEN_FUNCTION)
			is_attribute = css_ident_equal(&tokens[index], "attr");
		used = values_function_length(tokens + index, count - index);
		if (is_attribute) {
			name = values_skip_space(tokens, index + used, index + 1U);
			if (name < index + used && tokens[name].type == CSS_TOKEN_IDENT) {
				items[made].is_attribute = 1;
				items[made].text = vm_atom_from_units(parse->heap, tokens[name].text, tokens[name].length);
				if (items[made].text == NULL)
					return ENOMEM;
				made++;
			}
		}

		/* The next item (a function's arguments with it). */
		index += used;
	}

	/* The list, kept in the arena. */
	kept = wb_arena_zalloc(parse->arena, sizeof(*kept));
	if (kept == NULL)
		return ENOMEM;
	kept_items = wb_arena_alloc(parse->arena, made * sizeof(*kept_items) + 1U);
	if (kept_items == NULL)
		return ENOMEM;
	memcpy(kept_items, items, made * sizeof(*kept_items));
	kept->items = kept_items;
	kept->count = made;

	/* Succeeded: the content is a list. */
	value->kind = CSS_VALUE_CONTENT;
	value->content = kept;
	return 0;
}

/*
 * Parses the flex shorthand: none (0 0 auto), auto (1 1 auto), or a grow
 * factor, an optional shrink factor and an optional basis in any order
 * the grammar allows (a number alone makes the basis 0%).
 */
static int
values_flex(
	struct css_parse *parse,
	const struct css_token *tokens,
	size_t count,
	struct css_declaration *out,
	size_t *made)
{
	struct css_value grow;
	struct css_value shrink;
	struct css_value basis;
	struct css_value candidate;
	size_t starts[4];
	size_t lengths[4];
	size_t components;
	size_t index;
	size_t last_number;
	int numbers;
	int basis_set;
	int is_word;
	int error;

	/* The defaults of a flex value that names some parts: 1 1 0%. */
	basis_set = 0;
	memset(&grow, 0, sizeof(grow));
	grow.kind = CSS_VALUE_NUMBER;
	grow.number = 1;
	shrink = grow;
	memset(&basis, 0, sizeof(basis));
	basis.kind = CSS_VALUE_LENGTH;
	basis.number = 0;
	basis.unit = CSS_DUNIT_PERCENT;

	/* none and auto. */
	components = values_components(tokens, count, starts, lengths, 4);
	if (components == 1 && tokens[starts[0]].type == CSS_TOKEN_IDENT) {
		is_word = css_ident_equal(&tokens[starts[0]], "none");
		if (is_word) {
			grow.number = 0;
			shrink.number = 0;
		} else {
			is_word = css_ident_equal(&tokens[starts[0]], "auto");
		}

		/* Both keep an auto basis (another word is a basis alone). */
		if (is_word) {
			basis.kind = CSS_VALUE_KEYWORD;
			basis.keyword = CSS_UNIT_AUTO;
			values_add(out, made, CSS_PROP_FLEX_GROW, &grow);
			values_add(out, made, CSS_PROP_FLEX_SHRINK, &shrink);
			values_add(out, made, CSS_PROP_FLEX_BASIS, &basis);
			return 0;
		}
	}

	/* One to three parts: numbers are the factors in order, anything else the basis. */
	if (components == 0 || components > 3)
		return EINVAL;
	numbers = 0;
	last_number = 0;
	for (index = 0; index < components; index++) {
		/* The two factors are adjacent: a basis between them is invalid. */
		if (lengths[index] == 1 && tokens[starts[index]].type == CSS_TOKEN_NUMBER) {
			if (numbers > 0 && index != last_number + 1U)
				return EINVAL;
			last_number = index;
		}
		if (lengths[index] == 1 && tokens[starts[index]].type == CSS_TOKEN_NUMBER && tokens[starts[index]].number != 0) {
			if (numbers == 0)
				grow.number = (float)tokens[starts[index]].number;
			else if (numbers == 1)
				shrink.number = (float)tokens[starts[index]].number;
			else
				return EINVAL;
			numbers++;
			continue;
		}

		/* A zero is a factor where one is expected, else a basis. */
		if (lengths[index] == 1 && tokens[starts[index]].type == CSS_TOKEN_NUMBER && numbers < 2 && (numbers == 0 || index == 1)) {
			if (numbers == 0)
				grow.number = 0;
			else
				shrink.number = 0;
			numbers++;
			continue;
		}

		/* The basis, once (a calculation without units is not one). */
		if (basis_set || values_unitless_function(tokens + starts[index], lengths[index]))
			return EINVAL;
		error = values_single(parse, CSS_PROP_FLEX_BASIS, tokens + starts[index], lengths[index], &candidate);
		if (error != 0)
			return error;
		basis = candidate;
		basis_set = 1;
	}

	/* The three longhands. */
	values_add(out, made, CSS_PROP_FLEX_GROW, &grow);
	values_add(out, made, CSS_PROP_FLEX_SHRINK, &shrink);
	values_add(out, made, CSS_PROP_FLEX_BASIS, &basis);

	/* Succeeded: the flex longhands are declared. */
	return 0;
}

/*
 * Parses place-items, place-self or place-content: the align value, then
 * the justify value (the align value again when there is none; nothing
 * for place-content's baseline, which leaves justify-content initial).  A value may be two words (last baseline,
 * safe center).
 * https://drafts.csswg.org/css-align-3/#place-items-property
 */
static int
values_place(
	struct css_parse *parse,
	const struct css_token *tokens,
	size_t count,
	int align,
	int justify,
	struct css_declaration *out,
	size_t *made)
{
	size_t starts[4];
	size_t lengths[4];
	size_t components;
	size_t first_end;
	size_t second_end;
	size_t end;
	int error;

	/* The components, cut into the two values. */
	components = values_components(tokens, count, starts, lengths, 4);
	if (components == 0 || components > 4)
		return EINVAL;
	first_end = values_place_value(tokens, starts, components, 0);
	second_end = first_end;
	if (first_end < components)
		second_end = values_place_value(tokens, starts, components, first_end);
	if (second_end != components)
		return EINVAL;

	/* The align value. */
	end = starts[first_end - 1U] + lengths[first_end - 1U];
	error = css_parse_value_as(parse, align, tokens + starts[0], end - starts[0], out, made);
	if (error != 0)
		return error;

	/* The justify value: its own, or the align value's. */
	if (first_end < components) {
		end = starts[second_end - 1U] + lengths[second_end - 1U];
		error = css_parse_value_as(parse, justify, tokens + starts[first_end], end - starts[first_end], out, made);
	} else if (align == CSS_PROP_ALIGN_CONTENT && css_ident_equal(&tokens[starts[first_end - 1U]], "baseline")) {
		/* A baseline has no justify meaning: justify-content stays at its initial value. */
		error = 0;
	} else {
		error = css_parse_value_as(parse, justify, tokens + starts[0], end - starts[0], out, made);
	}
	return error;
}

/* The component after one value of a place shorthand that starts at first (a value is one or two words). */
static size_t
values_place_value(
	const struct css_token *tokens,
	const size_t *starts,
	size_t components,
	size_t first)
{
	const struct css_token *token;

	/* A prefix word takes the word after it. */
	token = &tokens[starts[first]];
	if (first + 1U < components && token->type == CSS_TOKEN_IDENT &&
	    (css_ident_equal(token, "first") || css_ident_equal(token, "last") ||
	    css_ident_equal(token, "safe") || css_ident_equal(token, "unsafe")))
		return first + 2U;

	/* One word. */
	return first + 1U;
}

/* Parses position-try: an optional position-try-order, then position-try-fallbacks. */
static int
values_position_try(
	struct css_parse *parse,
	const struct css_token *tokens,
	size_t count,
	struct css_declaration *out,
	size_t *made)
{
	struct css_value order;
	struct css_value fallbacks;
	size_t first;
	int keyword;
	int error;

	/* The order, when the first word is one. */
	memset(&order, 0, sizeof(order));
	order.kind = CSS_VALUE_NUMBER;
	first = 0;
	while (first < count && tokens[first].type == CSS_TOKEN_WHITESPACE)
		first++;
	if (first < count && css_try_order_parse(tokens + first, 1, &keyword) == 0) {
		order.number = (float)keyword;
		first++;
	}

	/* The fallbacks. */
	memset(&fallbacks, 0, sizeof(fallbacks));
	error = css_try_fallbacks_parse(parse, tokens + first, count - first, &fallbacks.try_list);
	if (error != 0)
		return error;
	fallbacks.kind = CSS_VALUE_TRY;

	/* Succeeded: both longhands. */
	values_add(out, made, CSS_PROP_POSITION_TRY_ORDER, &order);
	values_add(out, made, CSS_PROP_POSITION_TRY_FALLBACKS, &fallbacks);
	return 0;
}

/* Tells whether a component is a function whose arguments have no dimension or percentage (a number). */
static int
values_unitless_function(
	const struct css_token *tokens,
	size_t count)
{
	size_t index;

	/* A function. */
	if (count < 2U || tokens[0].type != CSS_TOKEN_FUNCTION)
		return 0;

	/* Without units. */
	for (index = 1; index < count; index++) {
		if (tokens[index].type == CSS_TOKEN_DIMENSION || tokens[index].type == CSS_TOKEN_PERCENTAGE)
			return 0;
	}

	/* Succeeded: the function gives a number. */
	return 1;
}

/* Parses flex-flow: a direction and a wrap, in either order, each optional. */
static int
values_flex_flow(
	struct css_parse *parse,
	const struct css_token *tokens,
	size_t count,
	struct css_declaration *out,
	size_t *made)
{
	struct css_value direction;
	struct css_value wrap;
	struct css_value candidate;
	size_t starts[3];
	size_t lengths[3];
	size_t components;
	size_t index;
	int error;

	/* The defaults: row, nowrap. */
	memset(&direction, 0, sizeof(direction));
	direction.kind = CSS_VALUE_KEYWORD;
	direction.keyword = CSS_FLEX_ROW;
	wrap = direction;
	wrap.keyword = CSS_FLEX_NOWRAP;

	/* Each part is one or the other. */
	components = values_components(tokens, count, starts, lengths, 3);
	if (components == 0 || components > 2)
		return EINVAL;
	for (index = 0; index < components; index++) {
		error = values_single(parse, CSS_PROP_FLEX_DIRECTION, tokens + starts[index], lengths[index], &candidate);
		if (error == 0) {
			direction = candidate;
			continue;
		}

		/* Or a wrap. */
		error = values_single(parse, CSS_PROP_FLEX_WRAP, tokens + starts[index], lengths[index], &candidate);
		if (error != 0)
			return error;
		wrap = candidate;
	}

	/* The two longhands. */
	values_add(out, made, CSS_PROP_FLEX_DIRECTION, &direction);
	values_add(out, made, CSS_PROP_FLEX_WRAP, &wrap);

	/* Succeeded: both declared. */
	return 0;
}

/*
 * Parses border-radius: one to four horizontal radii (top
 * left, top right, bottom right, bottom left, the missing ones copied as
 * the four-sided shorthands copy them), then optionally a slash and one to
 * four vertical radii (otherwise the same as the horizontal ones).
 */
static int
values_border_radius(
	struct css_parse *parse,
	const struct css_token *tokens,
	size_t count,
	struct css_declaration *out,
	size_t *made)
{
	struct css_value horizontal[4];
	struct css_value vertical[4];
	size_t slash;
	size_t corner;
	int error;

	/* The slash between the two lists, if there is one. */
	slash = count;
	for (corner = 0; corner < count; corner++) {
		if (tokens[corner].type == CSS_TOKEN_DELIM && tokens[corner].delim == '/') {
			slash = corner;
			break;
		}
	}

	/* The horizontal radii. */
	error = values_radii(parse, tokens, slash, horizontal);
	if (error != 0)
		return error;

	/* The vertical radii, or the horizontal ones again. */
	if (slash < count) {
		error = values_radii(parse, tokens + slash + 1U, count - slash - 1U, vertical);
		if (error != 0)
			return error;
	} else {
		memcpy(vertical, horizontal, sizeof(vertical));
	}

	/* Each corner's two longhands. */
	for (corner = 0; corner < 4U; corner++) {
		values_add(out, made, CSS_PROP_RADIUS_TOP_LEFT_X + 2 * (int)corner, &horizontal[corner]);
		values_add(out, made, CSS_PROP_RADIUS_TOP_LEFT_Y + 2 * (int)corner, &vertical[corner]);
	}

	/* Succeeded: eight longhands. */
	return 0;
}

/* Parses one to four radii into the four corners, the missing ones copied from their opposites. */
static int
values_radii(
	struct css_parse *parse,
	const struct css_token *tokens,
	size_t count,
	struct css_value *radii)
{
	size_t starts[5];
	size_t lengths[5];
	size_t components;
	size_t index;
	int error;

	/* One to four components, each a radius. */
	components = values_components(tokens, count, starts, lengths, 5);
	if (components == 0 || components > 4)
		return EINVAL;
	for (index = 0; index < components; index++) {
		error = values_single(parse, CSS_PROP_RADIUS_TOP_LEFT_X, tokens + starts[index], lengths[index], &radii[index]);
		if (error != 0)
			return error;
	}

	/* The missing ones, as margin copies its sides. */
	if (components < 2)
		radii[1] = radii[0];
	if (components < 3)
		radii[2] = radii[0];
	if (components < 4)
		radii[3] = radii[1];

	/* Succeeded: four radii. */
	return 0;
}

/* Parses one corner's radius: a horizontal radius and optionally a vertical one (otherwise the same). */
static int
values_corner_radius(
	struct css_parse *parse,
	int corner,
	const struct css_token *tokens,
	size_t count,
	struct css_declaration *out,
	size_t *made)
{
	/* The pair of longhands of the corner. */
	return values_pair(parse, CSS_PROP_RADIUS_TOP_LEFT_X + 2 * corner, CSS_PROP_RADIUS_TOP_LEFT_Y + 2 * corner, tokens, count, out, made);
}

/*
 * Parses box-shadow: none, or a comma-separated list of
 * shadows (the first CSS_SHADOWS of them are kept), in the parse's arena.
 */
static int
values_box_shadow(
	struct css_parse *parse,
	const struct css_token *tokens,
	size_t count,
	struct css_value *value)
{
	struct css_shadow_list *list;
	size_t start;
	size_t index;
	size_t used;
	int is_none;
	int error;

	/* The list, kept in the arena. */
	list = wb_arena_zalloc(parse->arena, sizeof(*list));
	if (list == NULL)
		return ENOMEM;
	value->kind = CSS_VALUE_SHADOWS;
	value->shadows = list;

	/* none is the empty list. */
	if (count == 1 && tokens[0].type == CSS_TOKEN_IDENT) {
		is_none = css_ident_equal(&tokens[0], "none");
		if (!is_none)
			return EINVAL;
		return 0;
	}

	/* Each shadow runs to a comma outside the functions. */
	start = 0;
	index = 0;
	while (index <= count) {
		if (index < count && tokens[index].type != CSS_TOKEN_COMMA) {
			used = 1;
			if (tokens[index].type == CSS_TOKEN_FUNCTION)
				used = values_function_length(tokens + index, count - index);
			index += used;
			continue;
		}

		/* One shadow from start to here, kept while there is room. */
		if (list->count < CSS_SHADOWS) {
			error = values_one_shadow(tokens + start, index - start, &list->shadows[list->count]);
			if (error != 0)
				return error;
			list->count++;
		}

		/* The next shadow starts after the comma. */
		index++;
		start = index;
	}

	/* Succeeded: the shadows are parsed. */
	return 0;
}

/* Parses one sharp text shadow, or none; blur and lists remain unsupported. */
static int
values_text_shadow(
	struct css_parse *parse,
	const struct css_token *tokens,
	size_t count,
	struct css_value *value)
{
	struct css_shadow_list *list;
	int error;

	list = wb_arena_zalloc(parse->arena, sizeof(*list));
	if (list == NULL)
		return ENOMEM;
	value->kind = CSS_VALUE_SHADOWS;
	value->shadows = list;
	if (count == 1 && tokens[0].type == CSS_TOKEN_IDENT && css_ident_equal(tokens, "none"))
		return 0;

	error = values_one_shadow(tokens, count, &list->shadows[0]);
	if (error != 0)
		return error;
	if (list->shadows[0].inset || list->shadows[0].lengths[2].number != 0.0f ||
	    list->shadows[0].lengths[3].number != 0.0f)
		return EINVAL;
	list->count = 1;
	return 0;
}

/* Parses one shadow: inset, two to four lengths and a color, the keyword and the color at either end. */
static int
values_one_shadow(
	const struct css_token *tokens,
	size_t count,
	struct css_declared_shadow *shadow)
{
	struct css_value candidate;
	size_t starts[7];
	size_t lengths[7];
	size_t components;
	size_t index;
	size_t lengths_seen;
	int is_inset;
	int error;

	/* No shadow is the default: black, all lengths zero pixels. */
	memset(shadow, 0, sizeof(*shadow));
	shadow->color = CSS_CURRENT_COLOR;
	for (index = 0; index < 4U; index++) {
		shadow->lengths[index].kind = CSS_VALUE_LENGTH;
		shadow->lengths[index].unit = CSS_DUNIT_PX;
	}

	/* Each component is inset, a length or a color. */
	components = values_components(tokens, count, starts, lengths, 7);
	if (components < 2 || components > 6)
		return EINVAL;
	lengths_seen = 0;
	for (index = 0; index < components; index++) {
		is_inset = 0;
		if (lengths[index] == 1 && tokens[starts[index]].type == CSS_TOKEN_IDENT)
			is_inset = css_ident_equal(&tokens[starts[index]], "inset");
		if (is_inset) {
			shadow->inset = 1;
			continue;
		}

		/* A length (four at most). */
		memset(&candidate, 0, sizeof(candidate));
		error = EINVAL;
		if (lengths[index] == 1 && lengths_seen < 4U)
			error = values_length(&tokens[starts[index]], 0, &candidate);
		if (error == 0) {
			shadow->lengths[lengths_seen] = candidate;
			lengths_seen++;
			continue;
		}

		/* Otherwise a color. */
		error = css_parse_color(tokens + starts[index], lengths[index], &shadow->color);
		if (error != 0)
			return error;
	}

	/* The two offsets are required. */
	if (lengths_seen < 2U)
		return EINVAL;

	/* Succeeded: the shadow is parsed. */
	return 0;
}

/* Parses the outline shorthand: a width, a style and a color in any order (medium, none and currentcolor when missing). */
static int
values_outline(
	struct css_parse *parse,
	const struct css_token *tokens,
	size_t count,
	struct css_declaration *out,
	size_t *made)
{
	struct css_value width;
	struct css_value style;
	struct css_value color;
	struct css_value candidate;
	size_t starts[4];
	size_t lengths[4];
	size_t components;
	size_t index;
	int error;

	/* The defaults: medium, none, currentcolor. */
	memset(&width, 0, sizeof(width));
	width.kind = CSS_VALUE_LENGTH;
	width.number = 3;
	width.unit = CSS_DUNIT_PX;
	memset(&style, 0, sizeof(style));
	style.kind = CSS_VALUE_KEYWORD;
	style.keyword = CSS_BORDER_NONE;
	memset(&color, 0, sizeof(color));
	color.kind = CSS_VALUE_COLOR;
	color.color = CSS_CURRENT_COLOR;

	/* Each component is a style, a width or a color. */
	components = values_components(tokens, count, starts, lengths, 4);
	if (components == 0 || components > 3)
		return EINVAL;
	for (index = 0; index < components; index++) {
		error = values_single(parse, CSS_PROP_OUTLINE_STYLE, tokens + starts[index], lengths[index], &candidate);
		if (error == 0) {
			style = candidate;
			continue;
		}

		/* Or a width. */
		error = values_single(parse, CSS_PROP_OUTLINE_WIDTH, tokens + starts[index], lengths[index], &candidate);
		if (error == 0) {
			width = candidate;
			continue;
		}

		/* Or a color. */
		error = values_single(parse, CSS_PROP_OUTLINE_COLOR, tokens + starts[index], lengths[index], &candidate);
		if (error == 0) {
			color = candidate;
			continue;
		}

		/* Anything else spoils the shorthand. */
		return EINVAL;
	}

	/* The three longhands. */
	values_add(out, made, CSS_PROP_OUTLINE_WIDTH, &width);
	values_add(out, made, CSS_PROP_OUTLINE_STYLE, &style);
	values_add(out, made, CSS_PROP_OUTLINE_COLOR, &color);

	/* Succeeded: the longhands are declared. */
	return 0;
}

/*
 * Parses clip-path: inset() with one to four lengths (the
 * missing ones copied as margin copies its sides; a "round" and its radii
 * after them are not kept) as an inset in the parse's arena; none, and the
 * shapes this pass does not clip by, as the keyword 0.
 */
static int
values_clip_path(
	struct css_parse *parse,
	const struct css_token *tokens,
	size_t count,
	struct css_value *value)
{
	struct css_declared_inset *inset;
	size_t starts[5];
	size_t lengths[5];
	size_t components;
	size_t used;
	size_t index;
	int is_inset;
	int is_round;
	int error;

	/* Anything but inset() clips nothing in this pass. */
	value->kind = CSS_VALUE_KEYWORD;
	value->keyword = 0;
	is_inset = 0;
	if (tokens[0].type == CSS_TOKEN_FUNCTION)
		is_inset = css_ident_equal(&tokens[0], "inset");
	if (!is_inset)
		return 0;

	/* The arguments, between the function's name and its closing parenthesis. */
	used = values_function_length(tokens, count);
	if (used < 2U)
		return EINVAL;
	components = values_components(tokens + 1, used - 2U, starts, lengths, 5);

	/* The inset, kept in the arena, zero pixels on every side to start with. */
	inset = wb_arena_zalloc(parse->arena, sizeof(*inset));
	if (inset == NULL)
		return ENOMEM;
	for (index = 0; index < 4U; index++) {
		inset->lengths[index].kind = CSS_VALUE_LENGTH;
		inset->lengths[index].unit = CSS_DUNIT_PX;
	}

	/* Up to four lengths, until round. */
	for (index = 0; index < components && index < 4U; index++) {
		is_round = 0;
		if (tokens[1U + starts[index]].type == CSS_TOKEN_IDENT)
			is_round = css_ident_equal(&tokens[1U + starts[index]], "round");
		if (is_round)
			break;
		if (lengths[index] != 1)
			return EINVAL;
		error = values_length(&tokens[1U + starts[index]], 0, &inset->lengths[index]);
		if (error != 0)
			return error;
	}

	/* No length at all is no inset. */
	if (index == 0)
		return EINVAL;

	/* The missing ones, as margin copies its sides. */
	if (index < 2U)
		inset->lengths[1] = inset->lengths[0];
	if (index < 3U)
		inset->lengths[2] = inset->lengths[0];
	if (index < 4U)
		inset->lengths[3] = inset->lengths[1];

	/* Succeeded: the inset is parsed. */
	value->kind = CSS_VALUE_INSET;
	value->inset = inset;
	return 0;
}

/*
 * Parses clip: auto, or rect() with four lengths or auto edges (top, right,
 * bottom, left; separated by commas or spaces) as an inset in the parse's
 * arena; anything else is an error.
 */
static int
values_clip(
	struct css_parse *parse,
	const struct css_token *tokens,
	size_t count,
	struct css_value *value)
{
	struct css_declared_inset *inset;
	size_t used;
	size_t index;
	size_t made;
	int error;

	/* auto: no clip. */
	value->kind = CSS_VALUE_KEYWORD;
	value->keyword = 0;
	if (tokens[0].type == CSS_TOKEN_IDENT && css_ident_equal(&tokens[0], "auto"))
		return 0;
	if (tokens[0].type != CSS_TOKEN_FUNCTION || !css_ident_equal(&tokens[0], "rect"))
		return EINVAL;

	/* The arguments, between the function's name and its closing parenthesis. */
	used = values_function_length(tokens, count);
	if (used < 2U)
		return EINVAL;
	inset = wb_arena_zalloc(parse->arena, sizeof(*inset));
	if (inset == NULL)
		return ENOMEM;
	made = 0;
	for (index = 1U; index + 1U < used; index++) {
		if (tokens[index].type == CSS_TOKEN_WHITESPACE || tokens[index].type == CSS_TOKEN_COMMA)
			continue;
		if (made == 4U)
			return EINVAL;
		error = values_length(&tokens[index], 1, &inset->lengths[made]);
		if (error != 0)
			return error;
		made++;
	}
	if (made != 4U)
		return EINVAL;

	/* Succeeded: the rectangle is parsed. */
	value->kind = CSS_VALUE_INSET;
	value->inset = inset;
	return 0;
}

/* Parses the auto-placement direction and optional dense packing. */
static int
values_grid_auto_flow(
	const struct css_token *tokens,
	size_t count,
	struct css_value *value)
{
	size_t index;
	int direction;
	int dense;

	direction = -1;
	dense = 0;
	for (index = 0; index < count; index++) {
		if (tokens[index].type == CSS_TOKEN_WHITESPACE)
			continue;
		if (tokens[index].type != CSS_TOKEN_IDENT)
			return EINVAL;
		if (css_ident_equal(&tokens[index], "row")) {
			if (direction >= 0)
				return EINVAL;
			direction = CSS_GRID_FLOW_ROW;
		} else if (css_ident_equal(&tokens[index], "column")) {
			if (direction >= 0)
				return EINVAL;
			direction = CSS_GRID_FLOW_COLUMN;
		} else if (css_ident_equal(&tokens[index], "dense")) {
			if (dense)
				return EINVAL;
			dense = CSS_GRID_FLOW_DENSE;
		} else {
			return EINVAL;
		}
	}
	if (direction < 0 && !dense)
		return EINVAL;
	value->kind = CSS_VALUE_KEYWORD;
	value->keyword = (direction == CSS_GRID_FLOW_COLUMN ? CSS_GRID_FLOW_COLUMN : CSS_GRID_FLOW_ROW) | dense;
	return 0;
}

/*
 * Parses a grid template: none, or a list of track sizes
 * (lengths, percentages, fr, auto, min-content and max-content,
 * minmax(), fit-content() as auto, and repeat() with a count), the line
 * names in brackets validated and passed by; kept in the parse's arena.
 */
static int
values_tracks(
	struct css_parse *parse,
	const struct css_token *tokens,
	size_t count,
	struct css_value *value)
{
	struct css_track_list *list;
	size_t index;
	int is_none;
	int error;

	/* The list, kept in the arena. */
	list = wb_arena_zalloc(parse->arena, sizeof(*list));
	if (list == NULL)
		return ENOMEM;
	value->kind = CSS_VALUE_TRACKS;
	value->tracks = list;

	/* none is the empty list. */
	if (count == 1 && tokens[0].type == CSS_TOKEN_IDENT) {
		is_none = css_ident_equal(&tokens[0], "none");
		if (is_none)
			return 0;
	}

	/* subgrid and its line names. */
	index = values_skip_space(tokens, count, 0);
	if (index < count && tokens[index].type == CSS_TOKEN_IDENT && css_ident_equal(&tokens[index], "subgrid"))
		return values_subgrid(parse, tokens + index + 1U, count - index - 1U, list);

	/* The tracks. */
	error = values_track_list(parse, tokens, count, list, 0);
	if (error != 0)
		return error;

	/* An empty list is not a template. */
	if (list->count == 0)
		return EINVAL;

	/* Succeeded: the template is parsed. */
	return 0;
}

/* Appends a token's text to a buffer as UTF-8. */
static int
values_token_utf8(
	const struct css_token *token,
	struct wb_buffer *out)
{
	size_t index;
	int error;

	/* Each unit (the identifiers of line names are not split across surrogates here). */
	for (index = 0; index < token->length; index++) {
		error = wb_buffer_append_utf8(out, token->text[index]);
		if (error != 0)
			return error;
	}
	return 0;
}

/* Parses one bracketed group of line names into its serialization ("[a b]"); *end is past it. */
static int
values_subgrid_names(
	struct css_parse *parse,
	const struct css_token *tokens,
	size_t count,
	size_t index,
	struct wb_buffer *out,
	size_t *end)
{
	struct vm_string *name;
	int first;
	int error;

	/* The group's names, each a custom identifier. */
	error = wb_buffer_append_byte(out, '[');
	first = 1;
	for (index++; error == 0 && index < count && tokens[index].type != CSS_TOKEN_CLOSE_SQUARE; index++) {
		if (tokens[index].type == CSS_TOKEN_WHITESPACE)
			continue;
		error = values_custom_ident(parse, &tokens[index], &name);
		if (error == 0 && !first)
			error = wb_buffer_append_byte(out, ' ');
		if (error == 0)
			error = values_token_utf8(&tokens[index], out);
		first = 0;
	}
	if (error != 0)
		return error;
	if (index == count)
		return EINVAL;

	/* Succeeded: the group is closed. */
	*end = index + 1U;
	return wb_buffer_append_byte(out, ']');
}

/*
 * Parses what follows subgrid in a grid template: line-name groups, and
 * repeat() of groups with a count or auto-fill (one at most), into the
 * value's serialization kept in the arena.  The subgrid has no tracks of
 * its own here.
 */
static int
values_subgrid(
	struct css_parse *parse,
	const struct css_token *tokens,
	size_t count,
	struct css_track_list *list)
{
	struct wb_buffer text;
	char *kept;
	size_t index;
	size_t end;
	size_t inner;
	size_t comma;
	int automatic;
	int groups;
	int error;

	/* subgrid, then each component after a space. */
	wb_buffer_init(&text);
	error = wb_buffer_append_string(&text, "subgrid");
	automatic = 0;
	index = 0;
	while (error == 0 && index < count) {
		if (tokens[index].type == CSS_TOKEN_WHITESPACE) {
			index++;
			continue;
		}
		error = wb_buffer_append_byte(&text, ' ');
		if (error != 0)
			break;

		/* A group of names. */
		if (tokens[index].type == CSS_TOKEN_OPEN_SQUARE) {
			error = values_subgrid_names(parse, tokens, count, index, &text, &index);
			continue;
		}

		/* repeat() of groups: a positive count or auto-fill, a comma, then groups only. */
		if (tokens[index].type != CSS_TOKEN_FUNCTION || !css_ident_equal(&tokens[index], "repeat")) {
			error = EINVAL;
			break;
		}
		end = index + values_function_length(tokens + index, count - index);
		inner = values_skip_space(tokens, end, index + 1U);
		error = wb_buffer_append_string(&text, "repeat(");
		if (error == 0 && inner < end && tokens[inner].type == CSS_TOKEN_IDENT && css_ident_equal(&tokens[inner], "auto-fill")) {
			if (automatic)
				error = EINVAL;
			automatic = 1;
			if (error == 0)
				error = wb_buffer_append_string(&text, "auto-fill");
		} else if (error == 0 && inner < end && tokens[inner].type == CSS_TOKEN_NUMBER && tokens[inner].integer &&
		    tokens[inner].number >= 1 && tokens[inner].number <= 10000) {
			error = wb_buffer_printf(&text, "%d", (int)tokens[inner].number);
		} else if (error == 0) {
			error = EINVAL;
		}
		comma = values_skip_space(tokens, end, inner + 1U);
		if (error == 0 && (comma >= end || tokens[comma].type != CSS_TOKEN_COMMA))
			error = EINVAL;
		if (error == 0)
			error = wb_buffer_append_byte(&text, ',');
		groups = 0;
		for (inner = comma + 1U; error == 0 && inner + 1U < end;) {
			if (tokens[inner].type == CSS_TOKEN_WHITESPACE) {
				inner++;
				continue;
			}
			if (tokens[inner].type != CSS_TOKEN_OPEN_SQUARE) {
				error = EINVAL;
				break;
			}
			error = wb_buffer_append_byte(&text, ' ');
			if (error == 0)
				error = values_subgrid_names(parse, tokens, end - 1U, inner, &text, &inner);
			groups++;
		}
		if (error == 0 && groups == 0)
			error = EINVAL;
		if (error == 0)
			error = wb_buffer_append_byte(&text, ')');
		index = end;
	}

	/* The serialization, kept in the arena. */
	if (error == 0) {
		kept = wb_arena_alloc(parse->arena, text.length + 1U);
		if (kept == NULL) {
			error = ENOMEM;
		} else {
			memcpy(kept, text.data, text.length);
			kept[text.length] = '\0';
			list->subgrid = kept;
		}
	}
	wb_buffer_release(&text);
	return error;
}

/* Parses an implicit track cycle through the same single-breadth grammar as explicit tracks. */
static int
values_auto_tracks(
	struct css_parse *parse,
	const struct css_token *tokens,
	size_t count,
	struct css_value *value)
{
	struct css_track_list *list;
	struct css_declared_track track;
	size_t index;
	size_t end;
	int error;

	/* Keeps the scalar cycle in the parse arena for ordinary cascade ownership. */
	list = wb_arena_zalloc(parse->arena, sizeof(*list));
	if (list == NULL)
		return ENOMEM;

	/* Consumes each single breadth while excluding repeated or named template syntax. */
	index = 0;
	while (index < count) {
		/* Whitespace alone cannot introduce another track. */
		index = values_skip_space(tokens, count, index);
		if (index == count)
			break;

		/* Bounded cycles use the same storage limit as explicit track lists. */
		if (list->count >= CSS_TRACKS)
			return EINVAL;

		/* A function forms one breadth; ordinary dimensions and keywords are one token. */
		end = index + 1U;
		if (tokens[index].type == CSS_TOKEN_FUNCTION)
			end = index + values_function_length(tokens + index, count - index);

		/* Incomplete functions cannot consume tokens beyond this declaration. */
		if (end <= index || end > count)
			return EINVAL;

		/* The common grammar retains intrinsic/minmax rules and rejects repeat or names. */
		error = values_one_track(parse, tokens + index, end - index, &track);
		if (error != 0)
			return error;

		/* Publishes one completely parsed breadth before consuming the next. */
		list->tracks[list->count] = track;
		list->count++;
		index = end;
	}

	/* An implicit cycle must contain at least one breadth. */
	if (list->count == 0U)
		return EINVAL;

	/* Succeeded: cascade receives the repeating scalar implicit-track pattern. */
	value->kind = CSS_VALUE_TRACKS;
	value->tracks = list;
	return 0;
}

/* Appends the tracks of a track list (repeat() expanded, one level deep) to a list, up to its room. */
static int
values_track_list(
	struct css_parse *parse,
	const struct css_token *tokens,
	size_t count,
	struct css_track_list *list,
	int depth)
{
	struct css_declared_track track;
	struct vm_string *name;
	size_t index;
	size_t end;
	size_t inner;
	size_t comma;
	size_t times;
	size_t time;
	size_t first;
	size_t added;
	int is_repeat;
	int last_was_names;
	int prefix;
	int body_end;
	int error;

	/* Walks the list: whitespace, bracketed names, repeat() and tracks. */
	index = 0;
	last_was_names = 0;
	while (index < count) {
		/* Whitespace between tracks. */
		if (tokens[index].type == CSS_TOKEN_WHITESPACE) {
			index++;
			continue;
		}

		/* One group of line names, kept on the line before the next track. */
		if (tokens[index].type == CSS_TOKEN_OPEN_SQUARE) {
			if (last_was_names)
				return EINVAL;
			index++;
			while (index < count && tokens[index].type != CSS_TOKEN_CLOSE_SQUARE) {
				if (tokens[index].type == CSS_TOKEN_WHITESPACE) {
					index++;
					continue;
				}

				/* Each name is a custom identifier. */
				error = values_custom_ident(parse, &tokens[index], &name);
				if (error != 0)
					return error;
				values_line_name(list, list->count, name);
				index++;
			}
			if (index == count)
				return EINVAL;
			index++;
			last_was_names = 1;
			continue;
		}

		/* One component: a function with its arguments, or one token. */
		end = index + 1U;
		if (tokens[index].type == CSS_TOKEN_FUNCTION)
			end = index + values_function_length(tokens + index, count - index);

		/* repeat(count, tracks), expanded (auto-fill and auto-fit are not read in this pass). */
		is_repeat = 0;
		if (tokens[index].type == CSS_TOKEN_FUNCTION)
			is_repeat = css_ident_equal(&tokens[index], "repeat");
		if (is_repeat) {
			if (depth > 0)
				return EINVAL;
			inner = values_skip_space(tokens, end, index + 1U);
			if (inner < end && tokens[inner].type == CSS_TOKEN_IDENT &&
			    (css_ident_equal(&tokens[inner], "auto-fill") || css_ident_equal(&tokens[inner], "auto-fit"))) {
				/* One automatic repeat per list; layout chooses its count. */
				if (list->repeat_length != 0)
					return EINVAL;
				comma = inner + 1U + values_split_at(tokens + inner + 1U, end - inner - 1U, CSS_TOKEN_COMMA, 0);
				if (comma >= end)
					return EINVAL;
				first = list->count;
				prefix = 0;
				if (first <= CSS_TRACKS)
					prefix = list->names[first].count;
				error = values_track_list(parse, tokens + comma + 1U, end - comma - 2U, list, depth + 1);
				if (error != 0)
					return error;
				added = list->count - first;
				if (added == 0)
					return EINVAL;

				/* Which names of the repeat's first and last lines are its own (the ones around it follow later). */
				list->repeat_name_prefix = prefix;
				list->repeat_name_end = 0;
				if (list->count <= CSS_TRACKS)
					list->repeat_name_end = list->names[list->count].count;

				/* Every repeated track needs a definite size for layout to count the repeats. */
				for (inner = first; inner < list->count; inner++) {
					if (list->tracks[inner].kind != CSS_TRACK_LENGTH &&
					    list->tracks[inner].minimum_kind != CSS_TRACK_LENGTH)
						return EINVAL;
				}
				list->repeat_at = (int)first;
				list->repeat_length = (int)added;
				list->repeat_fit = css_ident_equal(&tokens[values_skip_space(tokens, end, index + 1U)], "auto-fit");
				index = end;
				last_was_names = 0;
				continue;
			}
			if (inner >= end || tokens[inner].type != CSS_TOKEN_NUMBER || !tokens[inner].integer || tokens[inner].number < 1)
				return EINVAL;
			times = (size_t)tokens[inner].number;
			comma = inner + 1U + values_split_at(tokens + inner + 1U, end - inner - 1U, CSS_TOKEN_COMMA, 0);
			if (comma >= end)
				return EINVAL;

			/* The tracks once, then again until the count (or the room) runs out; their line names with them. */
			first = list->count;
			prefix = 0;
			if (first <= CSS_TRACKS)
				prefix = list->names[first].count;
			error = values_track_list(parse, tokens + comma + 1U, end - comma - 2U, list, depth + 1);
			if (error != 0)
				return error;
			added = list->count - first;
			if (added == 0)
				return EINVAL;
			body_end = 0;
			if (first + added <= CSS_TRACKS)
				body_end = list->names[first + added].count;
			for (time = 1; time < times && added != 0; time++) {
				for (inner = 0; inner < added && list->count < CSS_TRACKS; inner++) {
					list->tracks[list->count] = list->tracks[first + inner];

					/*
					 * The repeated lines' names: the body's own at its first line, all at the inner
					 * ones, and at its last only the body's own (not the next repetition's first ones).
					 */
					if (inner == 0)
						values_copy_line_names(list, first, list->count, prefix, CSS_LINE_NAMES);
					values_copy_line_names(list, first + inner + 1U, list->count + 1U, 0,
					    inner + 1U == added ? body_end : CSS_LINE_NAMES);
					list->count++;
				}
			}

			/* On past the function. */
			index = end;
			last_was_names = 0;
			continue;
		}

		/* One track. */
		error = values_one_track(parse, tokens + index, end - index, &track);
		if (error != 0)
			return error;
		if (list->count < CSS_TRACKS) {
			list->tracks[list->count] = track;
			list->count++;
		}

		/* On past it. */
		index = end;
		last_was_names = 0;
	}

	/* Succeeded: the tracks are appended. */
	return 0;
}

/* Adds a name to one line of a track list (past the room it is dropped). */
static void
values_line_name(
	struct css_track_list *list,
	size_t line,
	struct vm_string *name)
{
	struct css_line_names *names;

	/* Lines past the last track the list keeps are not kept either. */
	if (line > CSS_TRACKS)
		return;

	/* The name after the line's others. */
	names = &list->names[line];
	if (names->count < CSS_LINE_NAMES) {
		names->names[names->count] = name;
		names->count++;
	}
	list->named = 1;
}

/* Appends the names of one line of a track list, from its skip-th to before its end-th, to another (a repeated track's lines). */
static void
values_copy_line_names(
	struct css_track_list *list,
	size_t from,
	size_t to,
	int skip,
	int end)
{
	int index;
	int count;

	/* Each name the source line has, as it is now. */
	if (from > CSS_TRACKS || to > CSS_TRACKS)
		return;
	count = list->names[from].count;
	if (count > end)
		count = end;
	for (index = skip; index < count; index++)
		values_line_name(list, to, list->names[from].names[index]);
}

/*
 * Parses grid-template-areas: none, or one string per row whose cell
 * tokens (names, or runs of full stops for no area) are separated by
 * whitespace; every row has the same number of cells and each name's
 * cells make one filled rectangle.  Kept in the parse's arena.
 */
static int
values_areas(
	struct css_parse *parse,
	const struct css_token *tokens,
	size_t count,
	struct css_value *value)
{
	struct css_grid_areas *areas;
	size_t index;
	int columns;
	int is_none;
	int error;

	/* none is no areas. */
	value->kind = CSS_VALUE_AREAS;
	value->areas = NULL;
	if (count == 1 && tokens[0].type == CSS_TOKEN_IDENT) {
		is_none = css_ident_equal(&tokens[0], "none");
		if (is_none)
			return 0;
	}

	/* The cells, kept in the arena. */
	areas = wb_arena_zalloc(parse->arena, sizeof(*areas));
	if (areas == NULL)
		return ENOMEM;

	/* Each string is a row. */
	for (index = 0; index < count; index++) {
		if (tokens[index].type == CSS_TOKEN_WHITESPACE)
			continue;
		if (tokens[index].type != CSS_TOKEN_STRING)
			return EINVAL;
		error = values_area_row(parse, &tokens[index], areas, &columns);
		if (error != 0)
			return error;
	}

	/* At least one row, and each name a rectangle. */
	if (areas->rows == 0)
		return EINVAL;
	error = values_areas_check(areas);
	if (error != 0)
		return error;

	/* Succeeded: the areas are parsed. */
	value->areas = areas;
	return 0;
}

/*
 * Appends one row of grid-template-areas from its string: its cells,
 * which must be as many as the earlier rows'; columns is set to them.
 */
static int
values_area_row(
	struct css_parse *parse,
	const struct css_token *token,
	struct css_grid_areas *areas,
	int *columns)
{
	struct vm_string *name;
	const uint16_t *text;
	size_t length;
	size_t at;
	size_t start;
	uint16_t unit;
	int cells;
	int is_name;

	/* Refuses more rows than the areas keep. */
	if (areas->rows >= CSS_AREA_TRACKS)
		return EINVAL;

	/* Walks the string's tokens. */
	text = token->text;
	length = token->length;
	at = 0;
	cells = 0;
	while (at < length) {
		unit = text[at];

		/* Whitespace separates the tokens. */
		if (unit == ' ' || unit == '\t' || unit == '\n' || unit == '\r' || unit == '\f') {
			at++;
			continue;
		}

		/* A run of full stops is a cell of no area; a run of name characters names one. */
		start = at;
		if (unit == '.') {
			while (at < length && text[at] == '.')
				at++;
			name = NULL;
		} else {
			while (at < length) {
				is_name = values_area_name_unit(text[at]);
				if (!is_name)
					break;
				at++;
			}

			/* Any other character makes the value invalid. */
			if (at == start)
				return EINVAL;
			name = vm_atom_from_units(parse->heap, text + start, at - start);
			if (name == NULL)
				return ENOMEM;
		}

		/* The cell, while the row has room. */
		if (cells >= CSS_AREA_TRACKS)
			return EINVAL;
		areas->cells[areas->rows * CSS_AREA_TRACKS + cells] = name;
		cells++;
	}

	/* A row has cells, as many as the others. */
	if (cells == 0)
		return EINVAL;
	if (areas->rows > 0 && cells != areas->columns)
		return EINVAL;

	/* Succeeded: the row is appended. */
	areas->columns = cells;
	areas->rows++;
	*columns = cells;
	return 0;
}

/* Tells whether a UTF-16 unit may be part of an area's name (an identifier code point). */
static int
values_area_name_unit(
	uint16_t unit)
{
	/* Letters, digits, the hyphen, the low line, and anything past ASCII. */
	if (unit >= 'a' && unit <= 'z')
		return 1;
	if (unit >= 'A' && unit <= 'Z')
		return 1;
	if (unit >= '0' && unit <= '9')
		return 1;
	if (unit == '-' || unit == '_')
		return 1;
	if (unit >= 0x80)
		return 1;

	/* Anything else ends the name. */
	return 0;
}

/* Checks that the cells of each name in grid-template-areas make one filled rectangle. */
static int
values_areas_check(
	const struct css_grid_areas *areas)
{
	struct vm_string *name;
	int row;
	int column;
	int top;
	int left;
	int bottom;
	int right;
	int cells;
	int inner_row;
	int inner_column;

	/* Each name, from the first cell it has (read in order, so that cell is its area's top left). */
	for (row = 0; row < areas->rows; row++) {
		for (column = 0; column < areas->columns; column++) {
			name = areas->cells[row * CSS_AREA_TRACKS + column];
			if (name == NULL)
				continue;

			/* A name met before was checked at its first cell. */
			if (values_area_seen(areas, name, row, column))
				continue;

			/* The rectangle the name's cells span, and how many cells it has. */
			top = row;
			left = column;
			bottom = row;
			right = column;
			cells = 0;
			for (inner_row = 0; inner_row < areas->rows; inner_row++) {
				for (inner_column = 0; inner_column < areas->columns; inner_column++) {
					if (areas->cells[inner_row * CSS_AREA_TRACKS + inner_column] != name)
						continue;
					cells++;
					if (inner_row > bottom)
						bottom = inner_row;
					if (inner_column < left)
						left = inner_column;
					if (inner_column > right)
						right = inner_column;
				}
			}

			/* The cells fill the rectangle. */
			if (cells != (bottom - top + 1) * (right - left + 1))
				return EINVAL;
		}
	}

	/* Succeeded: every name is a rectangle. */
	return 0;
}

/* Tells whether a name occurs in grid-template-areas before a cell (in row order). */
static int
values_area_seen(
	const struct css_grid_areas *areas,
	struct vm_string *name,
	int row,
	int column)
{
	int index;
	int end;

	/* The cells before this one. */
	end = row * areas->columns + column;
	for (index = 0; index < end; index++) {
		if (areas->cells[(index / areas->columns) * CSS_AREA_TRACKS + index % areas->columns] == name)
			return 1;
	}

	/* None of them has the name. */
	return 0;
}

/* Classifies a parsed minmax lower bound without collapsing its intrinsic keyword. */
static int
values_track_minimum_rule(
	const struct css_token *token,
	int kind)
{
	int same;

	/* Resolved lengths retain their existing cascade path and explicit lower bound. */
	if (kind == CSS_TRACK_LENGTH)
		return CSS_TRACK_MINIMUM_LENGTH;

	/* The existing parser has already accepted one of the three intrinsic keywords. */
	same = css_ident_equal(token, "min-content");
	if (same)
		return CSS_TRACK_MINIMUM_MIN_CONTENT;

	/* Max-content has a different intrinsic minimum from either auto or min-content. */
	same = css_ident_equal(token, "max-content");
	if (same)
		return CSS_TRACK_MINIMUM_MAX_CONTENT;

	/* Succeeded: the remaining accepted lower bound is the automatic minimum. */
	return CSS_TRACK_MINIMUM_AUTO;
}

/* Classifies an accepted intrinsic maximum without changing the track grammar. */
static int
values_track_maximum_rule(
	const struct css_token *token,
	int kind)
{
	int same;

	/* Lengths and flexible breadths retain their existing coarse kind. */
	if (kind != CSS_TRACK_AUTO)
		return CSS_TRACK_MAXIMUM_DEFAULT;

	/* A min-content maximum must not measure or stretch as an automatic breadth. */
	same = css_ident_equal(token, "min-content");
	if (same)
		return CSS_TRACK_MAXIMUM_MIN_CONTENT;

	/* A max-content maximum has the unbroken contribution but no auto stretch. */
	same = css_ident_equal(token, "max-content");
	if (same)
		return CSS_TRACK_MAXIMUM_MAX_CONTENT;

	/* Succeeded: the accepted ordinary intrinsic maximum is automatic. */
	return CSS_TRACK_MAXIMUM_DEFAULT;
}

/* Parses one track while preserving the minimum and maximum breadth rules. */
static int
values_one_track(
	struct css_parse *parse,
	const struct css_token *tokens,
	size_t count,
	struct css_declared_track *track)
{
	struct css_value limit;
	size_t comma;
	size_t end;
	size_t start;
	size_t used;
	int is_minmax;
	int is_fit;
	int is_calc;
	int error;

	/* An ordinary track has no explicit minimum or intrinsic maximum keyword. */
	memset(track, 0, sizeof(*track));
	track->minimum_kind = CSS_TRACK_AUTO;

	/* Initializes every fit-bound field before either parser fills its active unit. */
	memset(&limit, 0, sizeof(limit));

	/* Functional breadths use their existing grammar before keyword classification. */
	if (tokens[0].type == CSS_TOKEN_FUNCTION) {
		/* A calculated length is an ordinary breadth without an explicit lower bound. */
		is_calc = css_ident_equal(&tokens[0], "calc");
		if (is_calc) {
			error = values_track_size(parse, tokens, count, &track->kind, &track->size);
			if (error != 0)
				return error;

			/* Succeeded: the calculated breadth is parsed. */
			return 0;
		}

		/* Fit-content keeps its accepted grammar and existing automatic sizing path. */
		is_fit = css_ident_equal(&tokens[0], "fit-content");
		if (is_fit) {
			/* Its single length or percentage remains nonnegative. */
			if (count < 3U)
				return EINVAL;

			/* Excludes surrounding whitespace from the existing function argument. */
			start = values_skip_space(tokens, count - 1U, 1U);
			end = count - 1U;
			while (end > start && tokens[end - 1U].type == CSS_TOKEN_WHITESPACE)
				end--;

			/* Simple arguments use the ordinary nonnegative length grammar. */
			if (end - start == 1U) {
				error = values_length(&tokens[start], 0, &limit);
				if (error != 0)
					return EINVAL;

				/* Negative fit limits are not track breadths. */
				if (limit.number < 0)
					return EINVAL;
			} else {
				/* Longer accepted arguments must be a single complete calc function. */
				if (tokens[start].type != CSS_TOKEN_FUNCTION)
					return EINVAL;

				/* Keeps the existing calc acceptance without adding another function grammar. */
				is_calc = css_ident_equal(&tokens[start], "calc");
				if (!is_calc)
					return EINVAL;

				/* Rejects trailing tokens outside the accepted calculation. */
				used = values_function_length(tokens + start, end - start);
				if (used != end - start)
					return EINVAL;

				/* Validates and retains the calculation through the ordinary length parser. */
				error = values_calc(parse, CSS_PROP_GRID_TEMPLATE_COLUMNS, tokens + start, end - start, &limit);
				if (error != 0)
					return error;

				/* A fit limit is a length-percentage rather than another value kind. */
				if (limit.kind != CSS_VALUE_LENGTH)
					return EINVAL;
			}

			/* Succeeded: the fit limit remains on its existing layout path with its computed bound. */
			track->kind = CSS_TRACK_AUTO;
			track->maximum_rule = CSS_TRACK_MAXIMUM_FIT_CONTENT;
			track->size = limit;
			return 0;
		}

		/* Every remaining accepted track function is minmax. */
		is_minmax = css_ident_equal(&tokens[0], "minmax");
		if (!is_minmax || count < 3U)
			return EINVAL;

		/* Finds the lower breadth before the comma and excludes its leading spaces. */
		comma = 1U + values_split_at(tokens + 1, count - 2U, CSS_TOKEN_COMMA, 0);
		if (comma >= count - 1U)
			return EINVAL;

		/* Parses the existing lower-bound grammar before retaining its keyword. */
		start = values_skip_space(tokens, comma, 1U);
		error = values_track_size(parse, tokens + start, comma - start, &track->minimum_kind, &track->minimum);
		if (error != 0)
			return error;

		/* Flexible factors are never valid lower bounds. */
		if (track->minimum_kind == CSS_TRACK_FR)
			return EINVAL;

		/* Copies the semantic minimum before moving to the maximum argument. */
		track->minimum_rule = values_track_minimum_rule(&tokens[start], track->minimum_kind);
		start = values_skip_space(tokens, count - 1U, comma + 1U);
		error = values_track_size(parse, tokens + start, count - 1U - start, &track->kind, &track->size);
		if (error != 0)
			return error;

		/* Succeeded: both accepted bounds retain their distinct intrinsic meaning. */
		track->maximum_rule = values_track_maximum_rule(&tokens[start], track->kind);
		return 0;
	}

	/* Parses a plain breadth before interpreting its accepted intrinsic keyword. */
	error = values_track_size(parse, tokens, count, &track->kind, &track->size);
	if (error != 0)
		return error;

	/* Succeeded: a plain min-content or max-content breadth stays distinct from auto. */
	track->maximum_rule = values_track_maximum_rule(tokens, track->kind);
	return 0;
}

/* Parses a track size: fr, auto (min-content, max-content), or a length or percentage. */
static int
values_track_size(
	struct css_parse *parse,
	const struct css_token *tokens,
	size_t count,
	int *kind,
	struct css_value *size)
{
	int same;
	int error;

	/* A calculated length uses the existing length calculation parser. */
	while (count > 1U && tokens[count - 1U].type == CSS_TOKEN_WHITESPACE)
		count--;
	if (tokens[0].type == CSS_TOKEN_FUNCTION && css_ident_equal(&tokens[0], "calc")) {
		if (values_function_length(tokens, count) != count)
			return EINVAL;
		*kind = CSS_TRACK_LENGTH;
		error = values_calc(parse, CSS_PROP_GRID_TEMPLATE_COLUMNS, tokens, count, size);
		if (error != 0 || size->kind != CSS_VALUE_LENGTH)
			return EINVAL;
		return 0;
	}

	/* An ordinary size is one token. */
	if (count != 1)
		return EINVAL;
	memset(size, 0, sizeof(*size));

	/* A share of the free space. */
	if (tokens[0].type == CSS_TOKEN_DIMENSION) {
		same = css_ident_equal(&tokens[0], "fr");
		if (same) {
			if (tokens[0].number < 0)
				return EINVAL;
			*kind = CSS_TRACK_FR;
			size->kind = CSS_VALUE_NUMBER;
			size->number = (float)tokens[0].number;
			return 0;
		}
	}

	/* The content's size. */
	if (tokens[0].type == CSS_TOKEN_IDENT) {
		same = css_ident_equal(&tokens[0], "auto");
		if (!same)
			same = css_ident_equal(&tokens[0], "min-content");
		if (!same)
			same = css_ident_equal(&tokens[0], "max-content");
		if (!same)
			return EINVAL;
		*kind = CSS_TRACK_AUTO;
		return 0;
	}

	/* A length or a percentage. */
	*kind = CSS_TRACK_LENGTH;
	error = values_length(&tokens[0], 0, size);
	if (error != 0)
		return error;
	if (size->number < 0)
		return EINVAL;
	return 0;
}

/* Finds the first token of a type (and a delimiter's character) outside functions and brackets; count when none. */
static size_t
values_split_at(
	const struct css_token *tokens,
	size_t count,
	int type,
	uint32_t delim)
{
	size_t index;
	int depth;

	/* Walks the tokens, keeping count of the open functions and parentheses. */
	depth = 0;
	for (index = 0; index < count; index++) {
		if (tokens[index].type == CSS_TOKEN_FUNCTION || tokens[index].type == CSS_TOKEN_OPEN_PAREN) {
			depth++;
			continue;
		}

		/* A closing parenthesis ends one. */
		if (tokens[index].type == CSS_TOKEN_CLOSE_PAREN) {
			depth--;
			continue;
		}

		/* A token of the type at the top. */
		if (depth == 0 && tokens[index].type == type && (type != CSS_TOKEN_DELIM || tokens[index].delim == delim))
			return index;
	}

	/* None. */
	return count;
}

/*
 * Parses a grid line: auto, a custom identifier (a named line), a line
 * number other than zero (negative from the end) with an optional name,
 * or span with a count, a name or both; anything else is invalid.
 */
static int
values_grid_line(
	struct css_parse *parse,
	const struct css_token *tokens,
	size_t count,
	struct css_value *value)
{
	struct vm_string *name;
	size_t index;
	int is_span;
	int has_number;
	int number;
	int same;
	int error;

	/* Starts from auto. */
	value->kind = CSS_VALUE_GRID_LINE;
	value->number = 0;
	value->keyword = 0;
	value->ident = NULL;
	while (count > 0 && tokens[count - 1U].type == CSS_TOKEN_WHITESPACE)
		count--;
	index = values_skip_space(tokens, count, 0);
	if (index == count)
		return EINVAL;

	/* auto stands alone. */
	same = 0;
	if (tokens[index].type == CSS_TOKEN_IDENT)
		same = css_ident_equal(&tokens[index], "auto");
	if (same) {
		index = values_skip_space(tokens, count, index + 1U);
		if (index != count)
			return EINVAL;
		return 0;
	}

	/* Takes span, a number and a name, each at most once and in any order. */
	is_span = 0;
	has_number = 0;
	number = 0;
	name = NULL;
	while (index < count) {
		same = 0;
		if (tokens[index].type == CSS_TOKEN_IDENT)
			same = css_ident_equal(&tokens[index], "span");
		if (same) {
			/* span comes first or last, never between the number and the name. */
			if (is_span)
				return EINVAL;
			if ((has_number || name != NULL) && values_skip_space(tokens, count, index + 1U) != count)
				return EINVAL;
			is_span = 1;
		} else if (tokens[index].type == CSS_TOKEN_NUMBER) {
			/* One integer other than zero. */
			if (has_number || !tokens[index].integer || tokens[index].number == 0)
				return EINVAL;
			if (tokens[index].number > 10000 || tokens[index].number < -10000)
				number = tokens[index].number > 0 ? 10000 : -10000;
			else
				number = (int)tokens[index].number;
			has_number = 1;
		} else {
			/* One custom identifier. */
			if (name != NULL)
				return EINVAL;
			error = values_custom_ident(parse, &tokens[index], &name);
			if (error != 0)
				return error;
		}
		index = values_skip_space(tokens, count, index + 1U);
	}

	/* A span counts at least one line; a line names its number, its name or both. */
	if (is_span) {
		if (has_number && number < 1)
			return EINVAL;
		if (!has_number && name == NULL)
			return EINVAL;
		value->keyword = 1;
		if (has_number)
			value->keyword = number;
	} else {
		value->number = (float)number;
	}

	/* Succeeded: the line is parsed. */
	value->ident = name;
	return 0;
}

/* Parses a custom identifier: an identifier that is no CSS-wide keyword nor auto, span or default. */
static int
values_custom_ident(
	struct css_parse *parse,
	const struct css_token *token,
	struct vm_string **name)
{
	static const char *const reserved[] = {
		"auto", "span", "default", "inherit", "initial", "unset", "revert", "revert-layer", NULL
	};
	size_t index;
	int same;

	/* Only an identifier names something. */
	if (token->type != CSS_TOKEN_IDENT)
		return EINVAL;

	/* The reserved words are not names. */
	for (index = 0; reserved[index] != NULL; index++) {
		same = css_ident_equal(token, reserved[index]);
		if (same)
			return EINVAL;
	}

	/* The name, as an atom. */
	*name = vm_atom_from_units(parse->heap, token->text, token->length);
	if (*name == NULL)
		return ENOMEM;

	/* Succeeded: the identifier is a name. */
	return 0;
}

/* Tells whether a parsed grid line is a custom identifier alone, which a shorthand copies to the missing lines. */
static int
values_grid_line_is_name(
	const struct css_value *line)
{
	/* A name with no number and no span. */
	if (line->ident == NULL)
		return 0;
	if (line->keyword != 0 || line->number != 0)
		return 0;

	/* Succeeded: the line is a name alone. */
	return 1;
}

/* Parses grid-column or grid-row: a start line, then optionally a slash and an end line. */
static int
values_grid_pair(
	struct css_parse *parse,
	int start,
	int end,
	const struct css_token *tokens,
	size_t count,
	struct css_declaration *out,
	size_t *made)
{
	struct css_value first;
	struct css_value second;
	size_t slash;
	size_t index;
	int error;

	/* The start before the slash. */
	memset(&first, 0, sizeof(first));
	slash = values_split_at(tokens, count, CSS_TOKEN_DELIM, '/');
	error = values_grid_line(parse, tokens, slash, &first);
	if (error != 0)
		return error;

	/* The end after it, or the start's name alone, or auto. */
	memset(&second, 0, sizeof(second));
	second.kind = CSS_VALUE_GRID_LINE;
	if (slash < count) {
		index = values_skip_space(tokens, count, slash + 1U);
		error = values_grid_line(parse, tokens + index, count - index, &second);
		if (error != 0)
			return error;
	} else if (values_grid_line_is_name(&first)) {
		second.ident = first.ident;
	}

	/* The two longhands. */
	values_add(out, made, start, &first);
	values_add(out, made, end, &second);

	/* Succeeded: both are declared. */
	return 0;
}

/*
 * Parses grid-area: row start / column start / row end / column end.  A
 * missing column start or row end takes the row start's name alone, a
 * missing column end the column start's; otherwise a missing line is auto.
 */
static int
values_grid_area(
	struct css_parse *parse,
	const struct css_token *tokens,
	size_t count,
	struct css_declaration *out,
	size_t *made)
{
	static const int properties[4] = {
		CSS_PROP_GRID_ROW_START, CSS_PROP_GRID_COLUMN_START, CSS_PROP_GRID_ROW_END, CSS_PROP_GRID_COLUMN_END
	};
	static const int copied_from[4] = { -1, 0, 0, 1 };
	struct css_value lines[4];
	size_t start;
	size_t slash;
	size_t part;
	size_t given;
	int error;

	/* Each part up to its slash. */
	start = 0;
	given = 0;
	for (part = 0; part < 4U && start < count; part++) {
		memset(&lines[part], 0, sizeof(lines[part]));
		slash = start + values_split_at(tokens + start, count - start, CSS_TOKEN_DELIM, '/');
		error = values_grid_line(parse, tokens + start, slash - start, &lines[part]);
		if (error != 0)
			return error;
		given++;
		start = slash + 1U;
		if (slash < count && start >= count)
			return EINVAL;
	}

	/* More than four parts is not a grid area. */
	if (start < count)
		return EINVAL;

	/* The missing parts. */
	for (part = given; part < 4U; part++) {
		memset(&lines[part], 0, sizeof(lines[part]));
		lines[part].kind = CSS_VALUE_GRID_LINE;
		if (values_grid_line_is_name(&lines[copied_from[part]]))
			lines[part].ident = lines[copied_from[part]].ident;
	}

	/* The four longhands. */
	for (part = 0; part < 4U; part++)
		values_add(out, made, properties[part], &lines[part]);

	/* Succeeded: the four are declared. */
	return 0;
}

/* Parses the supported explicit grid shorthand as two complete track lists. */
static int
values_grid_explicit(
	struct css_parse *parse,
	const struct css_token *tokens,
	size_t count,
	struct css_declaration *out,
	size_t *made)
{
	struct css_value rows;
	struct css_value columns;
	size_t slash;
	size_t row_end;
	size_t column_start;
	int error;

	/* A single top-level slash separates the explicit row and column tracks. */
	slash = values_split_at(tokens, count, CSS_TOKEN_DELIM, '/');
	if (slash == count)
		return EINVAL;
	row_end = slash;
	/* Trailing whitespace is not part of a single `none` row value. */
	while (row_end > 0 && tokens[row_end - 1U].type == CSS_TOKEN_WHITESPACE)
		row_end--;
	column_start = values_skip_space(tokens, count, slash + 1U);
	if (row_end == 0 || column_start == count)
		return EINVAL;

	/* Both sides must parse before either longhand is declared. */
	memset(&rows, 0, sizeof(rows));
	error = values_single(parse, CSS_PROP_GRID_TEMPLATE_ROWS, tokens, row_end, &rows);
	if (error != 0)
		return error;
	memset(&columns, 0, sizeof(columns));
	error = values_single(
	    parse,
	    CSS_PROP_GRID_TEMPLATE_COLUMNS,
	    tokens + column_start,
	    count - column_start,
	    &columns);
	if (error != 0)
		return error;

	/* The established longhand cascade handles order and leaves gaps alone. */
	values_add(out, made, CSS_PROP_GRID_TEMPLATE_ROWS, &rows);
	values_add(out, made, CSS_PROP_GRID_TEMPLATE_COLUMNS, &columns);

	/* Succeeded: both explicit track lists are declared. */
	return 0;
}

/*
 * Parses grid-template: none; rows / columns as two track lists; or the
 * areas form, rows of line names, a string and an optional size, then
 * optionally a slash and the columns.  It declares the three longhands.
 */
static int
values_grid_template(
	struct css_parse *parse,
	const struct css_token *tokens,
	size_t count,
	struct css_declaration *out,
	size_t *made)
{
	struct css_value rows;
	struct css_value columns;
	struct css_value areas;
	size_t index;
	int is_none;
	int has_string;
	int error;

	/* The areas form has a string before any slash. */
	has_string = 0;
	for (index = 0; index < count; index++) {
		if (tokens[index].type == CSS_TOKEN_DELIM && tokens[index].delim == '/')
			break;
		if (tokens[index].type == CSS_TOKEN_STRING)
			has_string = 1;
	}
	if (has_string) {
		error = values_grid_template_areas(parse, tokens, count, out, made);
		if (error != 0)
			return error;

		/* Succeeded: the three longhands are declared. */
		return 0;
	}

	/* No areas in the other forms. */
	memset(&areas, 0, sizeof(areas));
	areas.kind = CSS_VALUE_AREAS;

	/* none alone resets both track lists; anything else is rows / columns. */
	is_none = 0;
	if (count == 1 && tokens[0].type == CSS_TOKEN_IDENT)
		is_none = css_ident_equal(&tokens[0], "none");
	if (!is_none) {
		error = values_grid_explicit(parse, tokens, count, out, made);
		if (error != 0)
			return error;
		values_add(out, made, CSS_PROP_GRID_TEMPLATE_AREAS, &areas);

		/* Succeeded: the three longhands are declared. */
		return 0;
	}

	/* A lone none resets both explicit track lists. */
	memset(&rows, 0, sizeof(rows));
	error = values_single(parse, CSS_PROP_GRID_TEMPLATE_ROWS, tokens, count, &rows);
	if (error != 0)
		return error;
	memset(&columns, 0, sizeof(columns));
	error = values_single(parse, CSS_PROP_GRID_TEMPLATE_COLUMNS, tokens, count, &columns);
	if (error != 0)
		return error;
	values_add(out, made, CSS_PROP_GRID_TEMPLATE_ROWS, &rows);
	values_add(out, made, CSS_PROP_GRID_TEMPLATE_COLUMNS, &columns);
	values_add(out, made, CSS_PROP_GRID_TEMPLATE_AREAS, &areas);

	/* Succeeded: the three longhands are declared. */
	return 0;
}

/*
 * Parses the areas form of grid-template: before the slash, rows of
 * [line names]? "string" track-size? [line names]?, the names between two
 * rows going to the line between them; after it an explicit track list
 * without repeat().
 */
static int
values_grid_template_areas(
	struct css_parse *parse,
	const struct css_token *tokens,
	size_t count,
	struct css_declaration *out,
	size_t *made)
{
	struct css_track_list *list;
	struct css_grid_areas *cells;
	struct css_value rows;
	struct css_value columns;
	struct css_value areas;
	struct vm_string *name;
	size_t slash;
	size_t index;
	size_t end;
	size_t column_start;
	int state;
	int columns_found;
	int error;

	/* The rows' track list and the areas, in the arena. */
	list = wb_arena_zalloc(parse->arena, sizeof(*list));
	if (list == NULL)
		return ENOMEM;
	cells = wb_arena_zalloc(parse->arena, sizeof(*cells));
	if (cells == NULL)
		return ENOMEM;

	/* Each part of the rows before the slash. */
	slash = values_split_at(tokens, count, CSS_TOKEN_DELIM, '/');
	state = VALUES_ROW_START;
	index = 0;
	while (index < slash) {
		/* Whitespace between the parts. */
		if (tokens[index].type == CSS_TOKEN_WHITESPACE) {
			index++;
			continue;
		}

		/* Line names go on the line after the rows so far: one group before a row, one after it. */
		if (tokens[index].type == CSS_TOKEN_OPEN_SQUARE) {
			if (state == VALUES_ROW_LEADING_NAMES)
				return EINVAL;
			if (state == VALUES_ROW_STRING || state == VALUES_ROW_SIZE) {
				state = VALUES_ROW_TRAILING_NAMES;
			} else {
				state = VALUES_ROW_LEADING_NAMES;
			}
			index++;
			while (index < slash && tokens[index].type != CSS_TOKEN_CLOSE_SQUARE) {
				if (tokens[index].type != CSS_TOKEN_WHITESPACE) {
					error = values_custom_ident(parse, &tokens[index], &name);
					if (error != 0)
						return error;
					values_line_name(list, list->count, name);
				}
				index++;
			}
			if (index == slash)
				return EINVAL;
			index++;
			continue;
		}

		/* A string starts a row, auto until a size follows it. */
		if (tokens[index].type == CSS_TOKEN_STRING) {
			if (list->count >= CSS_TRACKS)
				return EINVAL;
			error = values_area_row(parse, &tokens[index], cells, &columns_found);
			if (error != 0)
				return error;
			memset(&list->tracks[list->count], 0, sizeof(list->tracks[list->count]));
			list->tracks[list->count].kind = CSS_TRACK_AUTO;
			list->tracks[list->count].minimum_kind = CSS_TRACK_AUTO;
			list->count++;
			state = VALUES_ROW_STRING;
			index++;
			continue;
		}

		/* A size belongs to the row just before it, and to no other. */
		if (state != VALUES_ROW_STRING)
			return EINVAL;
		end = index + 1U;
		if (tokens[index].type == CSS_TOKEN_FUNCTION)
			end = index + values_function_length(tokens + index, slash - index);
		if (end > slash)
			return EINVAL;
		error = values_one_track(parse, tokens + index, end - index, &list->tracks[list->count - 1U]);
		if (error != 0)
			return error;
		state = VALUES_ROW_SIZE;
		index = end;
	}

	/* The rows end after a row (its size or its names), and each name's cells make one rectangle. */
	if (state == VALUES_ROW_START || state == VALUES_ROW_LEADING_NAMES)
		return EINVAL;
	if (cells->rows == 0)
		return EINVAL;
	error = values_areas_check(cells);
	if (error != 0)
		return error;

	/* The columns after the slash: a track list without repeat(), or none without one. */
	memset(&columns, 0, sizeof(columns));
	if (slash < count) {
		column_start = values_skip_space(tokens, count, slash + 1U);
		if (column_start == count)
			return EINVAL;
		for (index = column_start; index < count; index++) {
			if (tokens[index].type == CSS_TOKEN_FUNCTION && css_ident_equal(&tokens[index], "repeat"))
				return EINVAL;
		}
		error = values_tracks(parse, tokens + column_start, count - column_start, &columns);
		if (error != 0)
			return error;
		if (columns.tracks->count == 0)
			return EINVAL;
	} else {
		error = values_tracks_none(parse, &columns);
		if (error != 0)
			return error;
	}

	/* The three longhands. */
	memset(&rows, 0, sizeof(rows));
	rows.kind = CSS_VALUE_TRACKS;
	rows.tracks = list;
	memset(&areas, 0, sizeof(areas));
	areas.kind = CSS_VALUE_AREAS;
	areas.areas = cells;
	values_add(out, made, CSS_PROP_GRID_TEMPLATE_ROWS, &rows);
	values_add(out, made, CSS_PROP_GRID_TEMPLATE_COLUMNS, &columns);
	values_add(out, made, CSS_PROP_GRID_TEMPLATE_AREAS, &areas);

	/* Succeeded: the areas form is parsed. */
	return 0;
}

/* Makes the empty track list (none). */
static int
values_tracks_none(
	struct css_parse *parse,
	struct css_value *value)
{
	struct css_track_list *list;

	/* An empty list in the arena. */
	list = wb_arena_zalloc(parse->arena, sizeof(*list));
	if (list == NULL)
		return ENOMEM;

	/* Succeeded: the value is none. */
	memset(value, 0, sizeof(*value));
	value->kind = CSS_VALUE_TRACKS;
	value->tracks = list;
	return 0;
}

/*
 * Parses the grid shorthand: a grid-template value, or rows / auto-flow
 * (with dense) and implicit column sizes, or auto-flow (with dense) and
 * implicit row sizes / columns.  Every grid longhand it does not give goes
 * back to its initial value (the gaps are not among them).
 */
static int
values_grid(
	struct css_parse *parse,
	const struct css_token *tokens,
	size_t count,
	struct css_declaration *out,
	size_t *made)
{
	struct css_value initial;
	struct css_value flow;
	struct css_value tracks;
	size_t slash;
	size_t side;
	size_t side_count;
	size_t index;
	size_t rest;
	int auto_flow;
	int dense;
	int on_left;
	int same;
	int error;

	/* Where the slash is, and on which side auto-flow is. */
	slash = values_split_at(tokens, count, CSS_TOKEN_DELIM, '/');
	on_left = -1;
	for (index = 0; index < count; index++) {
		same = 0;
		if (tokens[index].type == CSS_TOKEN_IDENT)
			same = css_ident_equal(&tokens[index], "auto-flow");
		if (same) {
			on_left = index < slash;
			break;
		}
	}

	/* Without auto-flow it is a grid-template value, with the implicit longhands initial. */
	memset(&initial, 0, sizeof(initial));
	initial.kind = CSS_VALUE_INITIAL;
	if (on_left < 0) {
		error = values_grid_template(parse, tokens, count, out, made);
		if (error != 0)
			return error;
		values_add(out, made, CSS_PROP_GRID_AUTO_FLOW, &initial);
		values_add(out, made, CSS_PROP_GRID_AUTO_ROWS, &initial);
		values_add(out, made, CSS_PROP_GRID_AUTO_COLUMNS, &initial);

		/* Succeeded: the template and the implicit longhands are declared. */
		return 0;
	}

	/* The auto-flow form needs the slash. */
	if (slash == count)
		return EINVAL;

	/* The auto-flow side: auto-flow and dense in either order, then the implicit sizes. */
	side = 0;
	side_count = slash;
	if (!on_left) {
		side = slash + 1U;
		side_count = count;
	}
	auto_flow = 0;
	dense = 0;
	index = values_skip_space(tokens, side_count, side);
	while (index < side_count && tokens[index].type == CSS_TOKEN_IDENT) {
		same = css_ident_equal(&tokens[index], "auto-flow");
		if (same && !auto_flow) {
			auto_flow = 1;
		} else {
			same = css_ident_equal(&tokens[index], "dense");
			if (!same || dense)
				break;
			dense = 1;
		}
		index = values_skip_space(tokens, side_count, index + 1U);
	}
	if (!auto_flow)
		return EINVAL;
	rest = index;

	/* The flow: rows when auto-flow is on the rows' side, else columns. */
	memset(&flow, 0, sizeof(flow));
	flow.kind = CSS_VALUE_KEYWORD;
	flow.keyword = CSS_GRID_FLOW_ROW;
	if (!on_left)
		flow.keyword = CSS_GRID_FLOW_COLUMN;
	if (dense)
		flow.keyword |= CSS_GRID_FLOW_DENSE;

	/* The implicit sizes on the auto-flow side, auto when none are given. */
	memset(&tracks, 0, sizeof(tracks));
	tracks.kind = CSS_VALUE_INITIAL;
	while (side_count > rest && tokens[side_count - 1U].type == CSS_TOKEN_WHITESPACE)
		side_count--;
	if (rest < side_count) {
		error = values_auto_tracks(parse, tokens + rest, side_count - rest, &tracks);
		if (error != 0)
			return error;
	}

	/* The explicit track list on the other side, the other axis none. */
	if (on_left) {
		error = values_grid_side(parse, CSS_PROP_GRID_TEMPLATE_COLUMNS, tokens + slash + 1U, count - slash - 1U, out, made);
		if (error != 0)
			return error;
		values_add(out, made, CSS_PROP_GRID_TEMPLATE_ROWS, &initial);
		values_add(out, made, CSS_PROP_GRID_AUTO_ROWS, &tracks);
		values_add(out, made, CSS_PROP_GRID_AUTO_COLUMNS, &initial);
	} else {
		error = values_grid_side(parse, CSS_PROP_GRID_TEMPLATE_ROWS, tokens, slash, out, made);
		if (error != 0)
			return error;
		values_add(out, made, CSS_PROP_GRID_TEMPLATE_COLUMNS, &initial);
		values_add(out, made, CSS_PROP_GRID_AUTO_COLUMNS, &tracks);
		values_add(out, made, CSS_PROP_GRID_AUTO_ROWS, &initial);
	}
	values_add(out, made, CSS_PROP_GRID_TEMPLATE_AREAS, &initial);
	values_add(out, made, CSS_PROP_GRID_AUTO_FLOW, &flow);

	/* Succeeded: every grid longhand is declared. */
	return 0;
}

/* Parses the explicit track list of one side of the grid shorthand and declares it. */
static int
values_grid_side(
	struct css_parse *parse,
	int property,
	const struct css_token *tokens,
	size_t count,
	struct css_declaration *out,
	size_t *made)
{
	struct css_value list;
	size_t start;
	int error;

	/* The side without its surrounding whitespace. */
	start = values_skip_space(tokens, count, 0);
	while (count > start && tokens[count - 1U].type == CSS_TOKEN_WHITESPACE)
		count--;
	if (start == count)
		return EINVAL;

	/* Its track list. */
	memset(&list, 0, sizeof(list));
	error = values_single(parse, property, tokens + start, count - start, &list);
	if (error != 0)
		return error;

	/* Succeeded: the side is declared. */
	values_add(out, made, property, &list);
	return 0;
}

/* Parses the distinct text-decoration-line keywords into their computed bit set. */
static int
values_decoration_line(
	const struct css_token *tokens,
	size_t count,
	struct css_value *value)
{
	size_t index;
	int bit;
	int lines;

	lines = 0;
	for (index = 0; index < count; index++) {
		if (tokens[index].type == CSS_TOKEN_WHITESPACE)
			continue;
		if (css_ident_equal(&tokens[index], "none")) {
			if (count != 1U)
				return EINVAL;
			bit = 0;
		} else if (css_ident_equal(&tokens[index], "underline")) {
			bit = CSS_DECORATION_UNDERLINE;
		} else if (css_ident_equal(&tokens[index], "overline")) {
			bit = CSS_DECORATION_OVERLINE;
		} else if (css_ident_equal(&tokens[index], "line-through")) {
			bit = CSS_DECORATION_LINE_THROUGH;
		} else if (css_ident_equal(&tokens[index], "blink")) {
			bit = CSS_DECORATION_BLINK;
		} else if (css_ident_equal(&tokens[index], "spelling-error")) {
			if (count != 1U)
				return EINVAL;
			bit = CSS_DECORATION_SPELLING_ERROR;
		} else if (css_ident_equal(&tokens[index], "grammar-error")) {
			if (count != 1U)
				return EINVAL;
			bit = CSS_DECORATION_GRAMMAR_ERROR;
		} else {
			return EINVAL;
		}
		if (bit != 0 && (lines & bit) != 0)
			return EINVAL;
		lines |= bit;
	}

	/* Succeeded: no repeated lines, and error decorations occur alone. */
	value->kind = CSS_VALUE_KEYWORD;
	value->keyword = lines;
	return 0;
}

/* Parses every component of the text-decoration shorthand before publishing its longhands. */
static int
values_text_decoration(
	const struct css_token *tokens,
	size_t count,
	struct css_declaration *out,
	size_t *made)
{
	struct css_value line_value;
	struct css_value style_value;
	struct css_value color_value;
	struct css_value thickness_value;
	struct css_value part_value;
	size_t starts[8];
	size_t lengths[8];
	size_t components;
	size_t index;
	int lines_seen;
	int line_ended;
	int style_seen;
	int color_seen;
	int thickness_seen;
	int error;

	components = values_components(tokens, count, starts, lengths, 8);
	if (components == 0 || components >= 8)
		return EINVAL;
	memset(&line_value, 0, sizeof(line_value));
	memset(&style_value, 0, sizeof(style_value));
	memset(&color_value, 0, sizeof(color_value));
	memset(&thickness_value, 0, sizeof(thickness_value));
	line_value.kind = CSS_VALUE_KEYWORD;
	style_value.kind = CSS_VALUE_KEYWORD;
	style_value.keyword = CSS_DECORATION_SOLID;
	color_value.kind = CSS_VALUE_COLOR;
	color_value.color = CSS_CURRENT_COLOR;
	thickness_value.kind = CSS_VALUE_KEYWORD;
	lines_seen = 0;
	line_ended = 0;
	style_seen = 0;
	color_seen = 0;
	thickness_seen = 0;

	for (index = 0; index < components; index++) {
		const struct css_token *part;
		size_t length;
		int keyword;

		part = tokens + starts[index];
		length = lengths[index];
		error = values_decoration_line(part, length, &part_value);
		if (error == 0) {
			if (line_ended ||
			    (lines_seen && (part_value.keyword == 0 || line_value.keyword == 0 ||
			    (line_value.keyword & part_value.keyword) != 0 ||
			    line_value.keyword >= CSS_DECORATION_SPELLING_ERROR ||
			    part_value.keyword >= CSS_DECORATION_SPELLING_ERROR)))
				return EINVAL;
			line_value.keyword |= part_value.keyword;
			lines_seen = 1;
			continue;
		}
		if (lines_seen)
			line_ended = 1;

		if (length == 1U && values_keyword(values_decoration_styles, part, &keyword)) {
			if (style_seen)
				return EINVAL;
			style_value.keyword = keyword;
			style_seen = 1;
			continue;
		}
		if (length == 1U &&
		    (css_ident_equal(part, "auto") || css_ident_equal(part, "from-font") ||
		    part->type == CSS_TOKEN_DIMENSION || part->type == CSS_TOKEN_PERCENTAGE ||
		    (part->type == CSS_TOKEN_NUMBER && part->number == 0))) {
			if (thickness_seen)
				return EINVAL;
			if (css_ident_equal(part, "auto")) {
				thickness_value.keyword = 0;
			} else if (css_ident_equal(part, "from-font")) {
				thickness_value.keyword = 1;
			} else {
				error = values_length(part, 0, &thickness_value);
				if (error != 0 || thickness_value.number < 0)
					return EINVAL;
			}
			thickness_seen = 1;
			continue;
		}
		if (color_seen)
			return EINVAL;
		error = css_parse_color(part, length, &color_value.color);
		if (error != 0)
			return EINVAL;
		color_seen = 1;
	}

	/* Every omitted subproperty takes its initial value. */
	values_add(out, made, CSS_PROP_TEXT_DECORATION_LINE, &line_value);
	values_add(out, made, CSS_PROP_TEXT_DECORATION_THICKNESS, &thickness_value);
	values_add(out, made, CSS_PROP_TEXT_DECORATION_STYLE, &style_value);
	values_add(out, made, CSS_PROP_TEXT_DECORATION_COLOR, &color_value);
	return 0;
}

/* Appends a nonempty shorthand component with the CSS separator. */
static int
values_decoration_append(struct wb_units *out, const struct wb_units *part)
{
	int error;

	if (part->length == 0U)
		return 0;
	if (out->length != 0U) {
		error = wb_utf8_to_units((const unsigned char *)" ", 1U, out);
		if (error != 0)
			return error;
	}
	return wb_units_append(out, part->data, part->length);
}

/* Serializes one parsed text-decoration shorthand and its four longhands. */
int
css_decoration_shorthand_serialize(
	const uint16_t *units,
	size_t length,
	struct wb_units *shorthand,
	struct wb_units *line,
	struct wb_units *thickness,
	struct wb_units *style,
	struct wb_units *color)
{
	struct css_declaration declarations[4];
	struct css_token *tokens;
	struct wb_arena arena;
	size_t starts[8];
	size_t lengths[8];
	size_t token_count;
	size_t component_count;
	size_t made;
	size_t start;
	size_t end;
	size_t index;
	size_t style_index;
	int keyword;
	int error;

	wb_arena_init(&arena, 0);
	error = css_tokenize(&arena, units, length, &tokens, &token_count);
	if (error != 0)
		goto cleanup;
	if (token_count == 0U) {
		error = EINVAL;
		goto cleanup;
	}
	start = 0U;
	end = token_count - 1U;
	while (start < end && tokens[start].type == CSS_TOKEN_WHITESPACE)
		start++;
	while (end > start && tokens[end - 1U].type == CSS_TOKEN_WHITESPACE)
		end--;
	if (start == end) {
		error = EINVAL;
		goto cleanup;
	}

	/* The production shorthand parser is the single validity decision. */
	made = 0U;
	error = values_text_decoration(tokens + start, end - start, declarations, &made);
	if (error != 0)
		goto cleanup;
	if (made != 4U) {
		error = EINVAL;
		goto cleanup;
	}
	component_count = values_components(tokens + start, end - start, starts, lengths, 8U);
	if (component_count == 0U || component_count >= 8U) {
		error = EINVAL;
		goto cleanup;
	}

	/* Each longhand keeps its specified component, or its initial value. */
	error = css_decoration_line_serialize(declarations[0].value.keyword, line);
	if (error != 0)
		goto cleanup;
	error = wb_utf8_to_units((const unsigned char *)"auto", 4U, thickness);
	if (error != 0)
		goto cleanup;
	error = wb_utf8_to_units((const unsigned char *)"solid", 5U, style);
	if (error != 0)
		goto cleanup;
	error = wb_utf8_to_units((const unsigned char *)"currentcolor", 12U, color);
	if (error != 0)
		goto cleanup;

	for (index = 0U; index < component_count; index++) {
		const struct css_token *part;
		size_t part_start;
		size_t part_end;
		struct css_value parsed;
		struct wb_units *destination;

		part = tokens + start + starts[index];
		part_start = part->source_start;
		part_end = part[lengths[index] - 1U].source_end;
		if (values_decoration_line(part, lengths[index], &parsed) == 0)
			continue;
		if (lengths[index] == 1U && values_keyword(values_decoration_styles, part, &keyword)) {
			style->length = 0U;
			for (style_index = 0U; values_decoration_styles[style_index].name != NULL; style_index++) {
				if (values_decoration_styles[style_index].value == keyword)
					break;
			}
			error = wb_utf8_to_units((const unsigned char *)values_decoration_styles[style_index].name,
			    strlen(values_decoration_styles[style_index].name), style);
			if (error != 0)
				goto cleanup;
			continue;
		}
		if (lengths[index] == 1U &&
		    (css_ident_equal(part, "auto") || css_ident_equal(part, "from-font") ||
		    part->type == CSS_TOKEN_DIMENSION || part->type == CSS_TOKEN_PERCENTAGE ||
		    (part->type == CSS_TOKEN_NUMBER && part->number == 0))) {
			destination = thickness;
		} else {
			destination = color;
		}
		destination->length = 0U;
		error = wb_units_append(destination, units + part_start, part_end - part_start);
		if (error != 0)
			goto cleanup;
	}

	/* Initial components are omitted unless the entire value is initial. */
	if (declarations[0].value.keyword != 0 ||
	    (declarations[1].value.kind == CSS_VALUE_KEYWORD && declarations[1].value.keyword == 0 &&
	    declarations[2].value.keyword == CSS_DECORATION_SOLID &&
	    declarations[3].value.color == CSS_CURRENT_COLOR)) {
		error = values_decoration_append(shorthand, line);
		if (error != 0)
			goto cleanup;
	}
	if (!(declarations[1].value.kind == CSS_VALUE_KEYWORD && declarations[1].value.keyword == 0)) {
		error = values_decoration_append(shorthand, thickness);
		if (error != 0)
			goto cleanup;
	}
	if (declarations[2].value.keyword != CSS_DECORATION_SOLID) {
		error = values_decoration_append(shorthand, style);
		if (error != 0)
			goto cleanup;
	}
	if (declarations[3].value.color != CSS_CURRENT_COLOR)
		error = values_decoration_append(shorthand, color);

cleanup:
	wb_arena_release(&arena);
	return error;
}

/* Adds a translation to an axis of transform: lengths of the same unit sum; another unit is not applied. */
static void
values_transform_add(
	struct css_value *axis,
	const struct css_value *value)
{
	if (axis->number == 0) {
		*axis = *value;
		return;
	}
	if (axis->unit == value->unit)
		axis->number += value->number;
}

/*
 * Parses transform into the two translations of its longhands.  Only the
 * translate functions move a box (translate, translateX, translateY and
 * translate3d); every other function is accepted and does nothing here.
 */
static int
values_transform(
	const struct css_token *tokens,
	size_t count,
	struct css_declaration *out,
	size_t *made)
{
	struct css_value set;
	int functions;
	struct css_value axis[2];
	struct css_value scale[2];
	struct css_value value;
	const struct css_token *argument[3];
	double factor;
	size_t index;
	size_t end;
	size_t inner;
	size_t arguments;
	size_t depth;
	int which;
	int seen;
	int error;

	/* Both axes start at zero pixels. */
	memset(axis, 0, sizeof(axis));
	axis[0].kind = CSS_VALUE_LENGTH;
	axis[0].unit = CSS_DUNIT_PX;
	axis[1] = axis[0];
	scale[0] = axis[0];
	scale[0].number = 1.0f;
	scale[1] = scale[0];
	seen = 0;
	index = 0;
	functions = 0;
	while (index < count) {
		if (tokens[index].type == CSS_TOKEN_WHITESPACE) {
			index++;
			continue;
		}

		/* none. */
		if (tokens[index].type == CSS_TOKEN_IDENT && css_ident_equal(&tokens[index], "none") && !seen) {
			seen = 1;
			index++;
			continue;
		}
		if (tokens[index].type != CSS_TOKEN_FUNCTION)
			return EINVAL;
		functions++;

		/* The function's extent: up to its closing parenthesis. */
		depth = 1;
		end = index + 1U;
		while (end < count && depth > 0) {
			if (tokens[end].type == CSS_TOKEN_FUNCTION || tokens[end].type == CSS_TOKEN_OPEN_PAREN)
				depth++;
			if (tokens[end].type == CSS_TOKEN_CLOSE_PAREN)
				depth--;
			end++;
		}
		if (depth != 0)
			return EINVAL;

		/* Which function it is. */
		which = -1;
		if (css_ident_equal(&tokens[index], "translate"))
			which = 0;
		else if (css_ident_equal(&tokens[index], "translatex"))
			which = 1;
		else if (css_ident_equal(&tokens[index], "translatey"))
			which = 2;
		else if (css_ident_equal(&tokens[index], "translate3d"))
			which = 3;
		seen = 1;

		/* scale, scaleX, scaleY and scale3d: numbers (or percentages) that multiply. */
		if (css_ident_equal(&tokens[index], "scale") || css_ident_equal(&tokens[index], "scalex") ||
		    css_ident_equal(&tokens[index], "scaley") || css_ident_equal(&tokens[index], "scale3d")) {
			which = 0;
			if (css_ident_equal(&tokens[index], "scalex"))
				which = 1;
			else if (css_ident_equal(&tokens[index], "scaley"))
				which = 2;
			arguments = 0;
			for (inner = index + 1U; inner + 1U < end; inner++) {
				if (tokens[inner].type == CSS_TOKEN_WHITESPACE || tokens[inner].type == CSS_TOKEN_COMMA)
					continue;
				if (arguments == 3U)
					return EINVAL;
				if (tokens[inner].type != CSS_TOKEN_NUMBER && tokens[inner].type != CSS_TOKEN_PERCENTAGE)
					return EINVAL;
				argument[arguments++] = &tokens[inner];
			}
			if (arguments == 0)
				return EINVAL;
			factor = argument[0]->number;
			if (argument[0]->type == CSS_TOKEN_PERCENTAGE)
				factor /= 100.0;
			if (which == 2) {
				scale[1].number *= (float)factor;
			} else {
				scale[0].number *= (float)factor;
				if (which == 0) {
					if (arguments > 1U) {
						factor = argument[1]->number;
						if (argument[1]->type == CSS_TOKEN_PERCENTAGE)
							factor /= 100.0;
					}
					scale[1].number *= (float)factor;
				}
			}
			index = end;
			continue;
		}
		if (which >= 0) {
			/* Its arguments, separated by commas (whitespace around them is skipped). */
			arguments = 0;
			for (inner = index + 1U; inner + 1U < end; inner++) {
				if (tokens[inner].type == CSS_TOKEN_WHITESPACE || tokens[inner].type == CSS_TOKEN_COMMA)
					continue;
				if (arguments == 3U)
					return EINVAL;
				argument[arguments++] = &tokens[inner];
			}
			if (arguments == 0)
				return EINVAL;
			error = values_length(argument[0], 0, &value);
			if (error != 0)
				return error;
			if (which == 2) {
				values_transform_add(&axis[1], &value);
			} else {
				values_transform_add(&axis[0], &value);
				if (arguments > 1U && which != 1) {
					error = values_length(argument[1], 0, &value);
					if (error != 0)
						return error;
					values_transform_add(&axis[1], &value);
				}
			}
		}
		index = end;
	}
	if (!seen)
		return EINVAL;

	/* Succeeded: the two translations, and whether there is a transform. */
	memset(&set, 0, sizeof(set));
	set.kind = CSS_VALUE_NUMBER;
	set.number = functions > 0 ? 1.0f : 0.0f;
	values_add(out, made, CSS_PROP_TRANSFORM_SET, &set);
	values_add(out, made, CSS_PROP_TRANSFORM_X, &axis[0]);
	values_add(out, made, CSS_PROP_TRANSFORM_Y, &axis[1]);
	values_add(out, made, CSS_PROP_TRANSFORM_SCALE_X, &scale[0]);
	values_add(out, made, CSS_PROP_TRANSFORM_SCALE_Y, &scale[1]);
	return 0;
}

/*
 * Parses aspect-ratio: auto, a ratio (a number, optionally slash another),
 * or both in either order.  The ratio is 0 for none (a zero or infinite
 * ratio counts as none).
 */
static int
values_aspect_ratio(
	const struct css_token *tokens,
	size_t count,
	struct css_declaration *out,
	size_t *made)
{
	struct css_value ratio;
	struct css_value automatic;
	double numerator;
	double denominator;
	size_t index;
	int have_ratio;
	int have_auto;

	memset(&ratio, 0, sizeof(ratio));
	ratio.kind = CSS_VALUE_NUMBER;
	automatic = ratio;
	numerator = 0;
	denominator = 1;
	have_ratio = 0;
	have_auto = 0;
	for (index = 0; index < count; index++) {
		if (tokens[index].type == CSS_TOKEN_WHITESPACE)
			continue;
		if (tokens[index].type == CSS_TOKEN_IDENT && css_ident_equal(&tokens[index], "auto")) {
			if (have_auto)
				return EINVAL;
			have_auto = 1;
			continue;
		}
		if (tokens[index].type != CSS_TOKEN_NUMBER || have_ratio || tokens[index].number < 0)
			return EINVAL;
		have_ratio = 1;
		numerator = tokens[index].number;
		for (index++; index < count && tokens[index].type == CSS_TOKEN_WHITESPACE; index++)
			;
		if (index < count && tokens[index].type == CSS_TOKEN_DELIM && tokens[index].delim == '/') {
			for (index++; index < count && tokens[index].type == CSS_TOKEN_WHITESPACE; index++)
				;
			if (index >= count || tokens[index].type != CSS_TOKEN_NUMBER || tokens[index].number < 0)
				return EINVAL;
			denominator = tokens[index].number;
		} else {
			index--;
		}
	}
	if (!have_ratio && !have_auto)
		return EINVAL;
	if (have_ratio && numerator > 0 && denominator > 0)
		ratio.number = (float)(numerator / denominator);
	automatic.number = (have_auto || !have_ratio) ? 1.0f : 0.0f;
	values_add(out, made, CSS_PROP_ASPECT_RATIO, &ratio);
	values_add(out, made, CSS_PROP_ASPECT_AUTO, &automatic);
	return 0;
}

/* Parses one component of transform-origin: left, center or right (top and bottom on the other axis), or a length. */
static int
values_transform_origin_component(
	const struct css_token *token,
	int *vertical,
	int *axis,
	struct css_value *value)
{
	float percent;

	/* A keyword fixes its axis (center fits either). */
	*axis = -1;
	if (token->type == CSS_TOKEN_IDENT) {
		if (css_ident_equal(token, "left")) {
			percent = 0.0f;
			*axis = 0;
		} else if (css_ident_equal(token, "right")) {
			percent = 100.0f;
			*axis = 0;
		} else if (css_ident_equal(token, "top")) {
			percent = 0.0f;
			*axis = 1;
		} else if (css_ident_equal(token, "bottom")) {
			percent = 100.0f;
			*axis = 1;
		} else if (css_ident_equal(token, "center")) {
			percent = 50.0f;
		} else {
			return EINVAL;
		}
		*vertical = 0;
		value->kind = CSS_VALUE_LENGTH;
		value->unit = CSS_DUNIT_PERCENT;
		value->number = percent;
		return 0;
	}
	return values_length(token, 0, value);
}

/* Parses transform-origin: one or two components (a third, z, is accepted and ignored). */
static int
values_transform_origin(
	const struct css_token *tokens,
	size_t count,
	int first_property,
	struct css_declaration *out,
	size_t *made)
{
	struct css_value first;
	struct css_value second;
	const struct css_token *part[3];
	size_t index;
	size_t parts;
	int first_axis;
	int second_axis;
	int vertical;
	int error;

	/* The components. */
	parts = 0;
	for (index = 0; index < count; index++) {
		if (tokens[index].type == CSS_TOKEN_WHITESPACE)
			continue;
		if (parts == 3U)
			return EINVAL;
		part[parts++] = &tokens[index];
	}
	if (parts == 0)
		return EINVAL;
	vertical = 0;
	error = values_transform_origin_component(part[0], &vertical, &first_axis, &first);
	if (error != 0)
		return error;
	memset(&second, 0, sizeof(second));
	second.kind = CSS_VALUE_LENGTH;
	second.unit = CSS_DUNIT_PERCENT;
	second.number = 50.0f;
	second_axis = -1;
	if (parts > 1U) {
		error = values_transform_origin_component(part[1], &vertical, &second_axis, &second);
		if (error != 0)
			return error;
	}

	/* A vertical keyword first (or a horizontal one second) swaps the two. */
	if (first_axis == 1 || second_axis == 0) {
		if (first_axis == 0 || second_axis == 1)
			return EINVAL;
		values_add(out, made, first_property, &second);
		values_add(out, made, first_property + 1, &first);
		return 0;
	}
	values_add(out, made, first_property, &first);
	values_add(out, made, first_property + 1, &second);
	return 0;
}
