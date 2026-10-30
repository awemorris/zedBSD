/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The inside of the CSS module, shared by its files: the tokens of CSS
 * Syntax 3, the parsed rules, selectors and declarations, and the
 * property table.
 */

#ifndef KEILAND_BROWSER_CSS_INTERNAL_H
#define KEILAND_BROWSER_CSS_INTERNAL_H

#include "css/css.h"

/* Reserved currentcolor encoding, resolved by the cascade against the text color. */
#define CSS_CURRENT_COLOR 0x00000001U

/* How many families a declared font-family keeps. */
#define CSS_DECLARED_FAMILIES CSS_FAMILIES_MAX

/* The most arguments a min(), max() or clamp() keeps. */
#define CSS_CALC_ARGUMENTS 3

/*
 * The property of a custom property's declaration (--name: tokens): its
 * name is custom_name and its value the tokens raw.
 */
#define CSS_PROP_CUSTOM (-1)

/*
 * The property of a declaration whose value uses var(): its tokens raw
 * are parsed as pending_property (a longhand or a shorthand) once the
 * element's custom properties are known.
 */
#define CSS_PROP_PENDING (-2)

/* The origins of style sheets. */
#define CSS_ORIGIN_USER_AGENT 0
#define CSS_ORIGIN_AUTHOR 1

/*
 * The token types of CSS Syntax 3.
 */
enum css_token_type {
	CSS_TOKEN_IDENT,
	CSS_TOKEN_FUNCTION,
	CSS_TOKEN_AT_KEYWORD,
	CSS_TOKEN_HASH,
	CSS_TOKEN_STRING,
	CSS_TOKEN_BAD_STRING,
	CSS_TOKEN_URL,
	CSS_TOKEN_BAD_URL,
	CSS_TOKEN_DELIM,
	CSS_TOKEN_NUMBER,
	CSS_TOKEN_PERCENTAGE,
	CSS_TOKEN_DIMENSION,
	CSS_TOKEN_WHITESPACE,
	CSS_TOKEN_CDO,
	CSS_TOKEN_CDC,
	CSS_TOKEN_COLON,
	CSS_TOKEN_SEMICOLON,
	CSS_TOKEN_COMMA,
	CSS_TOKEN_OPEN_SQUARE,
	CSS_TOKEN_CLOSE_SQUARE,
	CSS_TOKEN_OPEN_PAREN,
	CSS_TOKEN_CLOSE_PAREN,
	CSS_TOKEN_OPEN_CURLY,
	CSS_TOKEN_CLOSE_CURLY,
	CSS_TOKEN_EOF
};

/*
 * The kinds of simple selector.
 */
enum css_simple_kind {
	CSS_SIMPLE_TYPE,
	CSS_SIMPLE_UNIVERSAL,
	CSS_SIMPLE_ID,
	CSS_SIMPLE_CLASS,
	CSS_SIMPLE_ATTRIBUTE,
	CSS_SIMPLE_PSEUDO_CLASS,
	CSS_SIMPLE_PSEUDO_ELEMENT,
	CSS_SIMPLE_NEVER
};

/*
 * How an attribute selector compares the value.
 */
enum css_attribute_match {
	CSS_MATCH_EXISTS,
	CSS_MATCH_EQUAL,
	CSS_MATCH_INCLUDES,
	CSS_MATCH_DASH,
	CSS_MATCH_PREFIX,
	CSS_MATCH_SUFFIX,
	CSS_MATCH_SUBSTRING
};

/*
 * The pseudo-classes this pass understands.
 */
enum css_pseudo_class {
	CSS_PSEUDO_ROOT,
	CSS_PSEUDO_FIRST_CHILD,
	CSS_PSEUDO_LAST_CHILD,
	CSS_PSEUDO_ONLY_CHILD,
	CSS_PSEUDO_EMPTY,
	CSS_PSEUDO_LINK,
	CSS_PSEUDO_VISITED,
	CSS_PSEUDO_ANY_LINK,
	CSS_PSEUDO_NEVER,
	CSS_PSEUDO_ACTIVE_VT,
	CSS_PSEUDO_ACTIVE_VT_TYPE,
	CSS_PSEUDO_NOT,
	CSS_PSEUDO_IS,
	CSS_PSEUDO_HAS,
	CSS_PSEUDO_NTH_CHILD,
	CSS_PSEUDO_NTH_LAST_CHILD,
	CSS_PSEUDO_NTH_OF_TYPE,
	CSS_PSEUDO_NTH_LAST_OF_TYPE,
	CSS_PSEUDO_FIRST_OF_TYPE,
	CSS_PSEUDO_LAST_OF_TYPE,
	CSS_PSEUDO_ONLY_OF_TYPE,
	CSS_PSEUDO_DISABLED,
	CSS_PSEUDO_ENABLED,
	CSS_PSEUDO_CHECKED,
	CSS_PSEUDO_PLACEHOLDER_SHOWN,
	CSS_PSEUDO_REQUIRED,
	CSS_PSEUDO_OPTIONAL,
	CSS_PSEUDO_ALWAYS,
	CSS_PSEUDO_LANG,
	CSS_PSEUDO_SCOPE,
	CSS_PSEUDO_SCOPE_NEST,
	CSS_PSEUDO_TARGET,
	CSS_PSEUDO_POPOVER_OPEN
};

/*
 * How a compound selector relates to the one on its left.
 */
enum css_combinator {
	CSS_COMBINATOR_NONE,
	CSS_COMBINATOR_DESCENDANT,
	CSS_COMBINATOR_CHILD,
	CSS_COMBINATOR_NEXT,
	CSS_COMBINATOR_SUBSEQUENT
};

/* What a calculation does with its sums. */
enum css_calc_operation {
	CSS_CALC_SUM,
	CSS_CALC_MIN,
	CSS_CALC_MAX,
	CSS_CALC_CLAMP
};

/* The kinds of declared value. */
enum css_value_kind {
	CSS_VALUE_KEYWORD,
	CSS_VALUE_LENGTH,
	CSS_VALUE_NUMBER,
	CSS_VALUE_COLOR,
	CSS_VALUE_FAMILIES,
	CSS_VALUE_URL,
	CSS_VALUE_INHERIT,
	CSS_VALUE_INITIAL,
	CSS_VALUE_UNSET,
	CSS_VALUE_CONTENT,
	CSS_VALUE_SHADOWS,
	CSS_VALUE_FILTERS,
	CSS_VALUE_INSET,
	CSS_VALUE_TRACKS,
	CSS_VALUE_GRID_LINE,
	CSS_VALUE_AREAS,
	CSS_VALUE_ANCHOR,
	CSS_VALUE_ANCHOR_NAMES,
	CSS_VALUE_GRADIENT,
	CSS_VALUE_TRY
};

/* The units a declared length can have (converted to pixels by the cascade). */
enum css_declared_unit {
	CSS_DUNIT_PX,
	CSS_DUNIT_EM,
	CSS_DUNIT_REM,
	CSS_DUNIT_EX,
	CSS_DUNIT_PERCENT,
	CSS_DUNIT_PT,
	CSS_DUNIT_PC,
	CSS_DUNIT_IN,
	CSS_DUNIT_CM,
	CSS_DUNIT_MM,
	CSS_DUNIT_VW,
	CSS_DUNIT_VH,
	CSS_DUNIT_FONT_KEYWORD,
	CSS_DUNIT_VMIN,
	CSS_DUNIT_VMAX,
	CSS_DUNIT_CALC,
	CSS_DUNIT_CQW,
	CSS_DUNIT_CQH
};

/*
 * The properties this pass knows (longhands; shorthands expand into
 * these while parsing).
 */
enum css_property {
	CSS_PROP_DISPLAY,
	CSS_PROP_POSITION,
	CSS_PROP_FLOAT,
	CSS_PROP_VISIBILITY,
	CSS_PROP_WIDTH,
	CSS_PROP_HEIGHT,
	CSS_PROP_MIN_WIDTH,
	CSS_PROP_MAX_WIDTH,
	CSS_PROP_MIN_HEIGHT,
	CSS_PROP_MAX_HEIGHT,
	CSS_PROP_MARGIN_TOP,
	CSS_PROP_MARGIN_RIGHT,
	CSS_PROP_MARGIN_BOTTOM,
	CSS_PROP_MARGIN_LEFT,
	CSS_PROP_PADDING_TOP,
	CSS_PROP_PADDING_RIGHT,
	CSS_PROP_PADDING_BOTTOM,
	CSS_PROP_PADDING_LEFT,
	CSS_PROP_BORDER_TOP_WIDTH,
	CSS_PROP_BORDER_RIGHT_WIDTH,
	CSS_PROP_BORDER_BOTTOM_WIDTH,
	CSS_PROP_BORDER_LEFT_WIDTH,
	CSS_PROP_BORDER_TOP_STYLE,
	CSS_PROP_BORDER_RIGHT_STYLE,
	CSS_PROP_BORDER_BOTTOM_STYLE,
	CSS_PROP_BORDER_LEFT_STYLE,
	CSS_PROP_BORDER_TOP_COLOR,
	CSS_PROP_BORDER_RIGHT_COLOR,
	CSS_PROP_BORDER_BOTTOM_COLOR,
	CSS_PROP_BORDER_LEFT_COLOR,
	CSS_PROP_COLOR,
	CSS_PROP_BACKGROUND_COLOR,
	CSS_PROP_FONT_SIZE,
	CSS_PROP_FONT_WEIGHT,
	CSS_PROP_FONT_STYLE,
	CSS_PROP_FONT_FAMILY,
	CSS_PROP_LINE_HEIGHT,
	CSS_PROP_TEXT_ALIGN,
	CSS_PROP_TEXT_INDENT,
	CSS_PROP_WHITE_SPACE,
	CSS_PROP_CURSOR,
	CSS_PROP_TEXT_DECORATION_LINE,
	CSS_PROP_TEXT_DECORATION_STYLE,
	CSS_PROP_TEXT_DECORATION_SKIP_INK,
	CSS_PROP_TEXT_DECORATION_COLOR,
	CSS_PROP_TEXT_DECORATION_THICKNESS,
	CSS_PROP_LIST_STYLE_TYPE,
	CSS_PROP_TOP,
	CSS_PROP_RIGHT,
	CSS_PROP_BOTTOM,
	CSS_PROP_LEFT,
	CSS_PROP_Z_INDEX,
	CSS_PROP_CLEAR,
	CSS_PROP_OVERFLOW,
	CSS_PROP_OVERFLOW_X,
	CSS_PROP_OVERFLOW_Y,
	CSS_PROP_BACKGROUND_IMAGE,
	CSS_PROP_BACKGROUND_REPEAT,
	CSS_PROP_BACKGROUND_ATTACHMENT,
	CSS_PROP_BACKGROUND_POSITION_X,
	CSS_PROP_BACKGROUND_POSITION_Y,
	CSS_PROP_BACKGROUND_SIZE_WIDTH,
	CSS_PROP_BACKGROUND_SIZE_HEIGHT,
	CSS_PROP_BOX_SIZING,
	CSS_PROP_CONTENT,
	CSS_PROP_FLEX_DIRECTION,
	CSS_PROP_FLEX_WRAP,
	CSS_PROP_JUSTIFY_CONTENT,
	CSS_PROP_JUSTIFY_ITEMS,
	CSS_PROP_ALIGN_ITEMS,
	CSS_PROP_ALIGN_CONTENT,
	CSS_PROP_ROW_GAP,
	CSS_PROP_COLUMN_GAP,
	CSS_PROP_FLEX_GROW,
	CSS_PROP_FLEX_SHRINK,
	CSS_PROP_FLEX_BASIS,
	CSS_PROP_ALIGN_SELF,
	CSS_PROP_JUSTIFY_SELF,
	CSS_PROP_ORDER,
	CSS_PROP_VERTICAL_ALIGN,
	CSS_PROP_RADIUS_TOP_LEFT_X,
	CSS_PROP_RADIUS_TOP_LEFT_Y,
	CSS_PROP_RADIUS_TOP_RIGHT_X,
	CSS_PROP_RADIUS_TOP_RIGHT_Y,
	CSS_PROP_RADIUS_BOTTOM_RIGHT_X,
	CSS_PROP_RADIUS_BOTTOM_RIGHT_Y,
	CSS_PROP_RADIUS_BOTTOM_LEFT_X,
	CSS_PROP_RADIUS_BOTTOM_LEFT_Y,
	CSS_PROP_RADIUS_START_START_X,
	CSS_PROP_RADIUS_START_START_Y,
	CSS_PROP_RADIUS_START_END_X,
	CSS_PROP_RADIUS_START_END_Y,
	CSS_PROP_RADIUS_END_END_X,
	CSS_PROP_RADIUS_END_END_Y,
	CSS_PROP_RADIUS_END_START_X,
	CSS_PROP_RADIUS_END_START_Y,
	CSS_PROP_OPACITY,
	CSS_PROP_BOX_SHADOW,
	CSS_PROP_TEXT_SHADOW,
	CSS_PROP_OUTLINE_WIDTH,
	CSS_PROP_OUTLINE_STYLE,
	CSS_PROP_OUTLINE_COLOR,
	CSS_PROP_OUTLINE_OFFSET,
	CSS_PROP_CLIP_PATH,
	CSS_PROP_CLIP,
	CSS_PROP_DIRECTION,
	CSS_PROP_WRITING_MODE,
	CSS_PROP_GRID_TEMPLATE_COLUMNS,
	CSS_PROP_GRID_TEMPLATE_ROWS,
	CSS_PROP_GRID_AUTO_FLOW,
	CSS_PROP_GRID_AUTO_COLUMNS,
	CSS_PROP_GRID_AUTO_ROWS,
	CSS_PROP_GRID_COLUMN_START,
	CSS_PROP_GRID_COLUMN_END,
	CSS_PROP_GRID_ROW_START,
	CSS_PROP_GRID_ROW_END,
	CSS_PROP_CONTAINER_TYPE,
	CSS_PROP_BORDER_SPACING_X,
	CSS_PROP_BORDER_SPACING_Y,
	CSS_PROP_BORDER_COLLAPSE,
	CSS_PROP_CAPTION_SIDE,
	CSS_PROP_BACKDROP_FILTER,
	CSS_PROP_WILL_CHANGE,
	CSS_PROP_FILTER,
	CSS_PROP_TEXT_TRANSFORM,
	CSS_PROP_MARGIN_INLINE_START,
	CSS_PROP_MARGIN_INLINE_END,
	CSS_PROP_MARGIN_BLOCK_START,
	CSS_PROP_MARGIN_BLOCK_END,
	CSS_PROP_TEXT_ORIENTATION,
	CSS_PROP_OVERFLOW_INLINE,
	CSS_PROP_OVERFLOW_BLOCK,
	CSS_PROP_TRANSFORM_X,
	CSS_PROP_TRANSFORM_Y,
	CSS_PROP_TRANSFORM_SCALE_X,
	CSS_PROP_TRANSFORM_SCALE_Y,
	CSS_PROP_TRANSFORM_ORIGIN_X,
	CSS_PROP_TRANSFORM_ORIGIN_Y,
	CSS_PROP_ASPECT_RATIO,
	CSS_PROP_ASPECT_AUTO,
	CSS_PROP_OBJECT_FIT,
	CSS_PROP_OBJECT_POSITION_X,
	CSS_PROP_OBJECT_POSITION_Y,
	CSS_PROP_GRID_TEMPLATE_AREAS,
	/*
	 * The logical sides and sizes, each group in the order inline-start,
	 * inline-end, block-start, block-end (or inline size, block size, and
	 * their minima and maxima): the cascade gives them their physical
	 * property once the writing mode and the direction are known.
	 */
	CSS_PROP_PADDING_INLINE_START,
	CSS_PROP_PADDING_INLINE_END,
	CSS_PROP_PADDING_BLOCK_START,
	CSS_PROP_PADDING_BLOCK_END,
	CSS_PROP_INSET_INLINE_START,
	CSS_PROP_INSET_INLINE_END,
	CSS_PROP_INSET_BLOCK_START,
	CSS_PROP_INSET_BLOCK_END,
	CSS_PROP_BORDER_INLINE_START_WIDTH,
	CSS_PROP_BORDER_INLINE_END_WIDTH,
	CSS_PROP_BORDER_BLOCK_START_WIDTH,
	CSS_PROP_BORDER_BLOCK_END_WIDTH,
	CSS_PROP_BORDER_INLINE_START_STYLE,
	CSS_PROP_BORDER_INLINE_END_STYLE,
	CSS_PROP_BORDER_BLOCK_START_STYLE,
	CSS_PROP_BORDER_BLOCK_END_STYLE,
	CSS_PROP_BORDER_INLINE_START_COLOR,
	CSS_PROP_BORDER_INLINE_END_COLOR,
	CSS_PROP_BORDER_BLOCK_START_COLOR,
	CSS_PROP_BORDER_BLOCK_END_COLOR,
	CSS_PROP_INLINE_SIZE,
	CSS_PROP_BLOCK_SIZE,
	CSS_PROP_MIN_INLINE_SIZE,
	CSS_PROP_MAX_INLINE_SIZE,
	CSS_PROP_MIN_BLOCK_SIZE,
	CSS_PROP_MAX_BLOCK_SIZE,
	CSS_PROP_ANCHOR_NAME,
	CSS_PROP_POSITION_ANCHOR,
	CSS_PROP_ANCHOR_SCOPE,
	CSS_PROP_TRANSFORM_SET,
	CSS_PROP_POSITION_AREA,
	CSS_PROP_POSITION_TRY_FALLBACKS,
	CSS_PROP_POSITION_TRY_ORDER,
	CSS_PROP_POSITION_VISIBILITY,
	CSS_PROP_VIEW_TRANSITION_NAME,
	CSS_PROP_VIEW_TRANSITION_CLASS,
	CSS_PROP_ZOOM,
	CSS_PROP_ANIMATION_NAME,
	CSS_PROP_ANIMATION_DURATION,
	CSS_PROP_ANIMATION_DELAY,
	CSS_PROP_ANIMATION_PLAY_STATE,
	CSS_PROP_ANIMATION_TIMING_FUNCTION,
	CSS_PROP_MIX_BLEND_MODE,
	CSS_PROP_COUNT
};

/*
 * One token: its type, its text (the name of an ident, function,
 * at-keyword or hash, the value of a string or URL, the unit of a
 * dimension; unescaped, in the sheet's arena), its number and, for a
 * delimiter, its code point.
 */
struct css_token {
	/* Original UTF16 half-open token extent, excluding skipped leading comments. */
	size_t source_start;
	size_t source_end;
	int type;
	const uint16_t *text;
	size_t length;
	double number;
	int integer;
	uint32_t delim;
	int hash_is_id;
};

/*
 * One simple selector: a name (an atom), and for attributes a value (an
 * atom), a comparison, and explicit attribute case flags.  A pseudo-class is
 * pseudo (CSS_PSEUDO_*); :not,
 * :is, :where and :has keep their argument selectors (in the sheet's
 * arena), the :nth-* ones the a and b of an+b.  A pseudo-element is pseudo
 * (CSS_PSEUDO_ELEMENT_*).
 */
struct css_simple {
	int kind;
	struct vm_string *name;
	struct vm_string *value;
	int match;
	int case_insensitive;
	int case_sensitive;
	int pseudo;
	const struct css_selector *arguments;
	size_t argument_count;
	int nth_a;
	int nth_b;
};

/*
 * One compound selector and the combinator that joins it to the compound
 * on its left.
 */
struct css_compound {
	struct css_simple *simples;
	size_t count;
	int combinator;
	int pseudo_element;
	/* A view transition pseudo-element's name (NULL for *) and the classes it asks for. */
	struct vm_string *pseudo_name;
	struct vm_string *pseudo_classes[2];
	int pseudo_class_count;
};

/*
 * A complex selector, compounds from left to right, and its specificity
 * (ids << 16 | classes << 8 | types).
 */
struct css_selector {
	struct css_compound *compounds;
	size_t count;
	uint32_t specificity;
};

/*
 * A sum of lengths of the kinds calc() mixes, each the number of its unit:
 * pixels (the absolute units converted), percentages, font-relative and
 * viewport-relative lengths, and at most one min(), max() or clamp()
 * inside it times a factor (nested, in the parse's arena).
 * The cascade turns it into pixels and a percentage the layout resolves.
 */
struct css_calc_sum {
	float px;
	float percent;
	/* A direct percentage keeps its type even after its coefficient becomes zero. */
	int has_percent;
	float em;
	float ex;
	float rem;
	float vw;
	float vh;
	float vmin;
	float vmax;
	float cqw;
	float cqh;
	const struct css_calc *nested;
	float nested_factor;
};

/*
 * A calc(), min(), max() or clamp() length: one sum, or the
 * least or greatest of several, or the middle one of three.
 */
struct css_calc {
	int operation;
	size_t count;
	struct css_calc_sum sums[CSS_CALC_ARGUMENTS];
};

/*
 * A declared value, as parsed: a keyword, a length, a number, a color or
 * font families.  A length whose unit is CSS_DUNIT_CALC is the
 * calculation calc points at (in the arena the value was parsed into).
 */
struct css_value {
	int kind;
	int keyword;
	float number;
	int unit;
	uint32_t color;
	struct vm_string *families[CSS_DECLARED_FAMILIES];
	int family_count;
	struct vm_string *url;
	const struct css_calc *calc;
	const struct css_content *content;
	const struct css_shadow_list *shadows;
	const struct css_filter_list *filters;
	const struct css_declared_inset *inset;
	const struct css_track_list *tracks;
	/* A grid line's name (an atom; NULL for none), and grid-template-areas' cells (none when NULL). */
	struct vm_string *ident;
	const struct css_grid_areas *areas;
	/* A sum with anchor functions, and the names of anchor-name or anchor-scope (in the parse's arena). */
	const struct css_anchor_expr *anchor;
	const struct css_anchor_names *anchor_names;
	/* A background image that is a gradient (in the parse's arena). */
	const struct css_gradient *gradient;
	/* position-try-fallbacks' entries (in the parse's arena). */
	const struct css_try_list *try_list;
};

/* One declared grid track: its kind, its size (a length, or the fr share) and minmax()'s minimum. */
struct css_declared_track {
	int kind;
	struct css_value size;
	int minimum_kind;
	struct css_value minimum;
	/* The semantic minimum survives the legacy coarse track kind until cascade. */
	int minimum_rule;
	/* The maximum keyword survives parsing before computed lengths are resolved. */
	int maximum_rule;
};

/* A declared grid template, kept in the parse's arena (none is an empty list). */
struct css_track_list {
	struct css_declared_track tracks[CSS_TRACKS];
	size_t count;
	/* The one auto-fill or auto-fit repeat(): where its tracks start, how many, and whether it is auto-fit. */
	int repeat_at;
	int repeat_length;
	int repeat_fit;
	/* How many names the repeat's first line has before its own, and how many of its last line's are its own. */
	int repeat_name_prefix;
	int repeat_name_end;
	/* The names of the count + 1 lines around the tracks, and whether any line has one. */
	struct css_line_names names[CSS_TRACKS + 1];
	int named;
	/* A subgrid's value as it serializes ("subgrid" and its line-name lists), NUL-terminated in the arena; NULL otherwise. */
	const char *subgrid;
	/* A template with a repeat(): its text as written (spaces normalized, lengths in ems unresolved), NUL-terminated in the arena; NULL otherwise. */
	const char *declared;
};

/* A declared clip-path: inset(): its four declared lengths, top, right, bottom, left. */
struct css_declared_inset {
	struct css_value lengths[4];
};

/*
 * One declared box shadow: its offset, blur and spread as
 * declared lengths (the missing ones zero pixels), its color and whether
 * it is inset.
 */
struct css_declared_shadow {
	struct css_value lengths[4];
	uint32_t color;
	int inset;
};

/* One declared filter function: its kind and its argument (a number, a percentage, a length or an angle in degrees). */
struct css_declared_filter {
	int kind;
	struct css_value amount;
};

/* A declared filter list, kept in the parse's arena (none is an empty list). */
struct css_filter_list {
	struct css_declared_filter filters[CSS_FILTERS];
	size_t count;
};

/* A declared box-shadow list, kept in the parse's arena (none is an empty list). */
struct css_shadow_list {
	struct css_declared_shadow shadows[CSS_SHADOWS];
	size_t count;
};

/*
 * One declaration: a property, its value and whether it is !important.
 */
struct css_declaration {
	int property;
	int important;
	struct css_value value;
	struct vm_string *custom_name;
	const struct css_token *raw;
	size_t raw_count;
	int pending_property;
};

/*
 * One custom property an element has: its name, its value's
 * tokens with every var() replaced (none: the property is invalid), and
 * the next one of the element's list, which ends in its parent's list.
 */
struct css_custom {
	struct vm_string *name;
	const struct css_token *tokens;
	size_t count;
	const struct css_custom *next;
	int invalid;
};

/*
 * One test of a media query: the feature it measures (media.c), how it
 * compares, and the value (pixels, a ratio, dppx, or 0 or 1).
 */
struct css_media_test {
	int feature;
	int comparison;
	float value;
};

/*
 * One media query: whether its media type is this browser's, its tests,
 * and whether not turns the answer round.
 */
struct css_media_query {
	int negate;
	int type_matches;
	struct css_media_test *tests;
	size_t test_count;
};

/*
 * A media query list: one of its queries must hold, and so must the list
 * it is nested in (parent, NULL at the top).
 */
struct css_media {
	struct css_media_query *queries;
	size_t query_count;
	const struct css_media *parent;
};

/*
 * What parsing a value needs: the heap the names are atoms of, and the
 * arena a calculation is kept in.
 */
struct css_parse {
	struct vm_heap *heap;
	struct wb_arena *arena;
};

/*
 * A style rule: its selectors and its declarations (in the sheet's arena).
 */
struct css_rule {
	struct css_selector *selectors;
	size_t selector_count;
	struct css_declaration *declarations;
	size_t declaration_count;
	const struct css_media *media;
	/* The @scope the rule is in (NULL for none). */
	const struct css_scope *scope;
};

/*
 * An @scope rule's scope: the selectors of its roots (none for an implicit
 * scope, whose root is the parent of the sheet's owner, or the root element)
 * and of its limits (none for no limit), and the @scope it is nested in
 * (NULL for none), in the sheet's arena.  An element is in the scope when
 * it is a root or below one, and neither it nor an element between it and
 * the root matches a limit.
 */
struct css_scope {
	const struct css_selector *roots;
	size_t root_count;
	const struct css_selector *limits;
	size_t limit_count;
	const struct css_scope *parent;
};

/*
 * One selector of the rule index: the rule and which of its selectors.
 */
struct css_index_entry {
	uint32_t rule;
	uint32_t selector;
};

/*
 * The run of index entries filed under one key (an atom; NULL marks an
 * empty slot of the table).
 */
struct css_index_bucket {
	struct vm_string *key;
	uint32_t start;
	uint32_t count;
};

/*
 * An open-addressed table of buckets by key; capacity is a power of two
 * (zero for a table with no key).
 */
struct css_index_table {
	struct css_index_bucket *slots;
	size_t capacity;
};

/*
 * The rule index of a sheet (index.c): its selectors grouped by the key
 * of their rightmost compound, the groups found by id, class or type, and
 * the run of selectors any element may match.  Each run is in rule order.
 */
struct css_rule_index {
	struct css_index_entry *entries;
	size_t entry_count;
	struct css_index_table ids;
	struct css_index_table classes;
	struct css_index_table tags;
	uint32_t universal_start;
	uint32_t universal_count;
};

/*
 * A parsed style sheet: its rules (each with the @media lists it is
 * nested in), the URLs its @import rules name (atoms, in order) and their
 * media lists, its rule index and the arena all of them live in.
 */
/* A @position-try rule: its name (an atom) and its declarations (in the sheet's arena). */
struct css_try_rule {
	struct vm_string *name;
	struct css_declaration *declarations;
	size_t declaration_count;
};

/* One keyframe of an @keyframes rule: its offset (0 to 1) and its declarations. */
struct css_keyframe {
	float offset;
	struct css_declaration *declarations;
	size_t declaration_count;
};

/* An @keyframes rule: its name (an atom) and its keyframes in the order written. */
struct css_keyframes_rule {
	struct vm_string *name;
	struct css_keyframe *frames;
	size_t frame_count;
};

struct css_sheet {
	struct wb_arena arena;
	struct css_rule *rules;
	size_t rule_count;
	struct vm_string **imports;
	struct css_media **import_media;
	size_t import_count;
	struct css_font_face *font_faces;
	size_t font_face_count;
	struct css_try_rule *try_rules;
	size_t try_rule_count;
	struct css_keyframes_rule *keyframes;
	size_t keyframe_count;
	struct css_rule_index index;
	int origin;
};

/*
 * A selector list a script gave (querySelector, matches and closest): its selectors and the arena they live in.  The names in it
 * are atoms, which live as long as the heap, so the list needs no tracing.
 */
struct css_query {
	struct wb_arena arena;
	struct css_selector *selectors;
	size_t count;
};

/* The user agent's style sheet (ua.c). */
extern const char css_user_agent_sheet[];

/* The tokenizer (tokenizer.c). */
int css_tokenize(struct wb_arena *arena, const uint16_t *units, size_t length, struct css_token **tokens, size_t *count);

/* The parser (parser.c). */
int css_parse_sheet(struct vm_heap *heap, const uint16_t *units, size_t length, int origin, struct css_sheet *sheet);
int css_parse_declarations(struct vm_heap *heap, struct wb_arena *arena, const uint16_t *units, size_t length, struct css_declaration **declarations, size_t *count);
void css_sheet_release(struct css_sheet *sheet);
int css_ident_equal(const struct css_token *token, const char *ascii);

/* Anchor positioning's values (anchor.c). */
int css_anchor_mentioned(const struct css_token *tokens, size_t count);
int css_anchor_parse(struct css_parse *parse, const struct css_token *tokens, size_t count, int sizes, const struct css_anchor_expr **expr);
int css_position_area_parse(const struct css_token *tokens, size_t count, int *packed);
int css_try_fallbacks_parse(struct css_parse *parse, const struct css_token *tokens, size_t count, const struct css_try_list **list);
int css_try_order_parse(const struct css_token *tokens, size_t count, int *order);
int css_position_visibility_parse(const struct css_token *tokens, size_t count, int *bits);
int css_anchor_names_parse(struct css_parse *parse, const struct css_token *tokens, size_t count, const struct css_anchor_names **names);
int css_units_equal_ascii(const uint16_t *units, size_t length, const char *ascii);

/* The rule index (index.c). */
int css_index_build(struct css_sheet *sheet);
const struct css_index_bucket *css_index_find(const struct css_index_table *table, const struct vm_string *key);

/* Media queries (media.c). */
int css_media_parse(struct wb_arena *arena, const struct css_token *tokens, size_t count, const struct css_media *parent, struct css_media **media);
int css_media_matches(const struct css_media *media, float width, float height);

/* Values (values.c). */
int css_property_lookup(const struct css_token *name);
int css_parse_value(struct css_parse *parse, const struct css_token *tokens, size_t count, const struct css_token *name, struct css_declaration *out, size_t *out_count, size_t out_capacity);
int css_parse_property(struct css_parse *parse, int property, const struct css_token *tokens, size_t count, struct css_declaration *out, size_t *out_count);
int css_parse_color(const struct css_token *tokens, size_t count, uint32_t *color);
int css_gradient_parse(struct css_parse *parse, const struct css_token *tokens, size_t count, const struct css_gradient **gradient);
int css_parse_value_as(struct css_parse *parse, int property, const struct css_token *tokens, size_t count, struct css_declaration *out, size_t *out_count);
int css_parse_font(struct css_parse *parse, const struct css_token *tokens, size_t count, struct css_declaration *out, size_t *out_count);

#endif
