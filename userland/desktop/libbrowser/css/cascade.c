/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The cascade: the engine that holds a document's style sheets, selector
 * matching, and the computation of an element's style from the matching
 * declarations, its style attribute and its parent's style.
 */

#include "css/internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The font size of the root before any style sets it, in pixels. */
#define CASCADE_DEFAULT_FONT_SIZE	16.0f

/*
 * The size medium stands for in the monospace family, as in Chromium: a
 * size on the keyword scale is scaled by it over the default size when the
 * family becomes monospace alone.
 */
#define CASCADE_MONOSPACE_FONT_SIZE	13.0f

/* The viewport the vw and vh units measure until the layout sets one. */
#define CASCADE_DEFAULT_VIEWPORT_WIDTH	1024.0f
#define CASCADE_DEFAULT_VIEWPORT_HEIGHT	768.0f

/* How deep var() references are followed before a cycle is assumed. */
#define CASCADE_VAR_DEPTH	16

/* How deep :has() looks below an element (the parser caps nesting too). */
#define CASCADE_HAS_DEPTH	512

/* The most declarations one pending declaration expands into (a shorthand's longhands). */
#define CASCADE_EXPANSION_MAX	16U

/* The precedence of a declaration by its origin and importance (higher wins). */
#define RANK_USER_AGENT		0
#define RANK_AUTHOR		1
#define RANK_AUTHOR_IMPORTANT	2
#define RANK_USER_AGENT_IMPORTANT	3

/*
 * One sheet of an engine, in cascade order: the sheet, and the same sheet
 * again in owned when the engine parsed it itself (the user agent's, and
 * text added with css_engine_add_sheet) and frees it; a sheet lent by the
 * page (css_engine_add_parsed) has owned NULL and outlives the engine.
 * media are the lists the sheet applies under (its <link>'s and its
 * @import rules'), all of which must hold.
 */
struct cascade_sheet {
	const struct css_sheet *sheet;
	struct css_sheet *owned;
	const struct css_media *media[CSS_MEDIA_CHAIN_MAX];
	size_t media_count;
};

/*
 * The style sheets of a document, the atoms the matching looks up, and
 * the buffers the matching of one element reuses: the atoms of its
 * classes, the index entries that may match it, and a class name's
 * characters on their way to an atom.  The arena holds what lives as long
 * as the engine: the media lists of <link> elements and the elements'
 * custom properties, which the computed styles point at.  pseudo_wanted is
 * the pseudo-element being styled (CSS_PSEUDO_ELEMENT_NONE for the element
 * itself), and pseudo_seen collects, while an element is styled, the bits
 * of the pseudo-elements some rule gives it.
 */
struct css_engine {
	struct vm_heap *heap;
	struct wb_vector sheets;
	struct vm_string *atom_id;
	struct vm_string *atom_class;
	struct vm_string *atom_style;
	struct vm_string *atom_href;
	float root_font_size;
	float viewport_width;
	float viewport_height;
	struct wb_vector class_keys;
	struct wb_vector candidates;
	struct wb_units word;
	struct wb_arena arena;
	int pseudo_wanted;
	int pseudo_seen;
	struct cascade_classes *class_table;
	size_t class_capacity;
	size_t class_count;
	struct wb_vector class_atoms;
	struct cascade_cached *style_table;
	size_t style_capacity;
	size_t style_count;
	css_container_lookup container_lookup;
	void *container_context;
	struct dom_element *current;
	int container_missed;
	struct wb_vector container_uses;
	int (*visited_lookup)(void *, struct dom_element *);
	void *visited_context;
	int visited_pass;
};

/*
 * One query container whose size the engine's styles were computed with;
 * the page checks it against each new layout.
 */
struct cascade_container_use {
	const struct dom_element *element;
	float width;
	float height;
};

/*
 * One element's computed style, kept by the engine so that a
 * layout again (an image or a font arrived) does not run the cascade for
 * every element again.  The engine is made anew when the document or its
 * sheets change, and forgets the styles when the viewport changes; an
 * element's parent style is the same whenever the engine computes it (the
 * layout and the style dump walk the tree alike).
 */
struct cascade_cached {
	struct dom_element *element;
	struct css_style *style;
};

/*
 * One class attribute's words as atoms, which the engine
 * keeps for as long as it lives so that a class selector compares
 * pointers instead of characters: the attribute's string (with its length
 * and hash, so that a string freed and another made at its address is
 * not mistaken for it), and the run of class_atoms that holds the words
 * some sheet names (a word no sheet names has no atom, and no selector
 * can name it).
 */
struct cascade_classes {
	struct vm_string *classes;
	uint32_t length;
	uint32_t hash;
	uint32_t start;
	uint32_t count;
};

/*
 * One declaration that applies to the element being styled, with what
 * orders it among the others: its sheet's place in the cascade, its
 * rule's place in the sheet and its own place in the rule, packed.
 */
struct cascade_match {
	const struct css_declaration *declaration;
	int rank;
	uint32_t specificity;
	uint64_t order;
};

/* One legacy keyword and the declaration it stands for. */
struct cascade_hint_keyword {
	const char *keyword;
	const char *declaration;
};

/* The legacy align and valign keyword mappings. */
static const struct cascade_hint_keyword cascade_table_align[] = {
	{ "center", "margin-left:auto;margin-right:auto;" },
	{ "left", "float:left;" },
	{ "right", "float:right;" },
	{ NULL, NULL }
};

static const struct cascade_hint_keyword cascade_text_align[] = {
	{ "left", "text-align:left;" },
	{ "right", "text-align:right;" },
	{ "center", "text-align:center;" },
	{ "justify", "text-align:justify;" },
	{ NULL, NULL }
};

static const struct cascade_hint_keyword cascade_vertical_align[] = {
	{ "top", "vertical-align:top;" },
	{ "middle", "vertical-align:middle;" },
	{ "bottom", "vertical-align:bottom;" },
	{ "baseline", "vertical-align:baseline;" },
	{ NULL, NULL }
};

static int cascade_collect(struct css_engine *engine, struct dom_element *element, struct wb_vector *matches, struct wb_arena *scratch);
static int cascade_presentational(struct css_engine *engine, struct dom_element *element, struct wb_vector *matches, struct wb_arena *scratch);
static int cascade_hint_ascii(struct wb_units *units, const char *text);
static int cascade_hint_length(struct wb_units *units, const char *property, const struct vm_string *value);
static int cascade_hint_color(struct wb_units *units, const char *property, const struct vm_string *value);
static int cascade_hint_equal(const struct vm_string *value, const char *keyword);
static int cascade_hint_keywords(struct wb_units *units, const struct vm_string *value, const struct cascade_hint_keyword *keywords);
static int cascade_element_keys(struct css_engine *engine, struct dom_element *element, struct vm_string **id);
static int cascade_add_bucket(struct css_engine *engine, const struct css_rule_index *index, const struct css_index_bucket *bucket);
static int cascade_collect_sheet(struct css_engine *engine, struct dom_element *element, const struct css_sheet *sheet, uint64_t sheet_number, struct vm_string *id, struct wb_vector *matches);
static int cascade_entry_compare(const void *left, const void *right);
static int cascade_add_declarations(struct wb_vector *matches, const struct css_declaration *declarations, size_t count, int origin, uint32_t specificity, uint64_t order);
static int cascade_compare(const void *left, const void *right);
static int cascade_customs(struct css_engine *engine, struct css_style *style, const struct cascade_match *matches, size_t count);
static int cascade_substitute(const struct css_custom *list, const struct css_token *tokens, size_t count, int depth, struct wb_vector *out);
static const struct css_custom *cascade_custom_find(const struct css_custom *list, const struct css_token *name);
static int cascade_keep_tokens(struct css_engine *engine, const struct css_token *tokens, size_t count, const struct css_token **kept);
static int cascade_resolve_pending(struct css_engine *engine, const struct css_style *style, struct wb_vector *list, struct wb_arena *scratch);
static struct css_length cascade_calc(struct css_engine *engine, const struct css_calc *calc, float font_size);
static float cascade_calc_pixels(struct css_engine *engine, const struct css_calc *calc, float font_size);
static void cascade_container(struct css_engine *engine, float *width, float *height);
static int cascade_candidate_matches(struct css_engine *engine, struct dom_element *element, const struct css_selector *selector);
static int cascade_selector_matches(struct css_engine *engine, struct dom_element *element, const struct css_selector *selector, size_t index, struct dom_element *anchor);
static int cascade_anchored(struct dom_element *element, int combinator, struct dom_element *anchor);
static int cascade_language(struct dom_element *element, const struct vm_string *range);
static int cascade_language_matches(const struct vm_string *language, const struct vm_string *range);
static int cascade_pseudo_class(struct css_engine *engine, struct dom_element *element, const struct css_simple *simple);
static int cascade_any_matches(struct css_engine *engine, struct dom_element *element, const struct css_simple *simple);
static int cascade_has(struct css_engine *engine, struct dom_element *element, const struct css_simple *simple);
static int cascade_has_below(struct css_engine *engine, struct dom_node *node, const struct css_simple *simple, struct dom_element *anchor, int depth);
static int cascade_any_relative(struct css_engine *engine, struct dom_element *element, const struct css_simple *simple, struct dom_element *anchor);
static int cascade_nth(struct dom_element *element, const struct css_simple *simple);
static int cascade_is_form_element(const struct dom_element *element);
static int cascade_compute(struct css_engine *engine, struct dom_element *element, const struct css_style *parent, struct css_style *style, int pseudo);
static int cascade_paint_color(struct css_engine *engine, struct dom_element *element, const struct css_style *parent, struct css_style *style, int pseudo);
static int cascade_compound_matches(struct css_engine *engine, struct dom_element *element, const struct css_compound *compound);
static int cascade_simple_matches(struct css_engine *engine, struct dom_element *element, const struct css_simple *simple);
static int cascade_attribute_matches(const struct vm_string *value, const struct css_simple *simple, int insensitive, int html_type);
static int cascade_has_class(const struct vm_string *classes, const struct vm_string *name);
static int cascade_class_atoms(struct css_engine *engine, struct vm_string *classes, struct vm_string *const **atoms, size_t *count);
static int cascade_class_grow(struct css_engine *engine);
static struct cascade_classes *cascade_class_slot(struct css_engine *engine, struct vm_string *classes);
static struct cascade_cached *cascade_style_slot(struct css_engine *engine, const struct dom_element *element);
static void cascade_style_keep(struct css_engine *engine, struct dom_element *element, const struct css_style *style);
static int cascade_style_grow(struct css_engine *engine);
static void cascade_style_forget(struct css_engine *engine);
static struct dom_element *cascade_parent_element(struct dom_element *element);
static struct dom_element *cascade_previous_element(struct dom_element *element);
static struct dom_element *cascade_next_element(struct dom_element *element);
static void cascade_apply(struct css_engine *engine, struct css_style *style, const struct css_style *parent, const struct css_declaration *declaration);
static int cascade_margin_property(int property, int writing_mode, int direction);
static void cascade_inherit(struct css_style *style, const struct css_style *parent, int property);
static struct css_length cascade_flex_basis(struct css_engine *engine, const struct css_value *value, float font_size);
static struct css_length cascade_length(struct css_engine *engine, const struct css_value *value, float font_size);
static void cascade_shadows(struct css_engine *engine, struct css_style *style, const struct css_value *value);
static void cascade_text_shadow(struct css_engine *engine, struct css_style *style, const struct css_value *value);
static void cascade_align(struct css_style *style, unsigned bit, int *field, int keyword);
static void cascade_tracks(struct css_engine *engine, struct css_style *style, const struct css_value *value, struct css_track *tracks, int *count, struct css_track_repeat *repeat);
static const struct css_line_names *cascade_line_names(const struct css_value *value);
static float cascade_font_size(struct css_engine *engine, const struct css_value *value, float parent_size);
static int cascade_font_size_keyword(const struct css_value *value, int parent_keyword);
static void cascade_families(struct css_style *style, const struct css_value *value);
static int cascade_monospace_only(const struct css_style *style);
static void cascade_monospace_size(struct css_style *style, const struct css_style *parent, const struct css_declaration *font_size);

/*
 * Makes an engine with the user agent's style sheet.
 */
int
css_engine_create(
	struct css_engine **engine,
	struct vm_heap *heap)
{
	struct css_engine *created;
	struct wb_units units;
	int error;

	/* Allocates the engine and interns the attribute names it looks up. */
	created = calloc(1, sizeof(*created));
	if (created == NULL)
		return ENOMEM;
	created->heap = heap;
	created->root_font_size = CASCADE_DEFAULT_FONT_SIZE;
	created->viewport_width = CASCADE_DEFAULT_VIEWPORT_WIDTH;
	created->viewport_height = CASCADE_DEFAULT_VIEWPORT_HEIGHT;
	wb_vector_init(&created->sheets, sizeof(struct cascade_sheet));
	wb_vector_init(&created->class_keys, sizeof(struct vm_string *));
	wb_vector_init(&created->candidates, sizeof(struct css_index_entry));
	wb_units_init(&created->word);
	wb_vector_init(&created->class_atoms, sizeof(struct vm_string *));
	wb_vector_init(&created->container_uses, sizeof(struct cascade_container_use));
	wb_arena_init(&created->arena, 0);
	created->atom_id = vm_atom_from_ascii(heap, "id");
	created->atom_class = vm_atom_from_ascii(heap, "class");
	created->atom_style = vm_atom_from_ascii(heap, "style");
	created->atom_href = vm_atom_from_ascii(heap, "href");
	if (created->atom_id == NULL || created->atom_class == NULL || created->atom_style == NULL || created->atom_href == NULL) {
		free(created);
		return ENOMEM;
	}

	/* Parses the user agent's sheet as the first sheet. */
	wb_units_init(&units);
	error = wb_utf8_to_units((const unsigned char *)css_user_agent_sheet, strlen(css_user_agent_sheet), &units);
	if (error == 0)
		error = css_engine_add_sheet_origin(created, units.data, units.length, CSS_ORIGIN_USER_AGENT);
	wb_units_release(&units);
	if (error != 0) {
		css_engine_destroy(created);
		return error;
	}

	/* Succeeded: the engine is ready for the author's sheets. */
	*engine = created;
	return 0;
}

/*
 * Destroys an engine and its style sheets.
 */
void
css_engine_destroy(
	struct css_engine *engine)
{
	struct cascade_sheet *sheets;
	size_t index;

	/* A NULL engine is nothing to destroy. */
	if (engine == NULL)
		return;

	/* Frees every sheet the engine parsed itself (the lent ones are their lender's). */
	sheets = engine->sheets.items;
	for (index = 0; index < engine->sheets.count; index++)
		css_sheet_destroy(sheets[index].owned);

	/* Frees the lists, the buffers and the engine. */
	wb_vector_release(&engine->sheets);
	wb_vector_release(&engine->class_keys);
	wb_vector_release(&engine->candidates);
	wb_units_release(&engine->word);
	wb_vector_release(&engine->class_atoms);
	free(engine->class_table);
	cascade_style_forget(engine);
	free(engine->style_table);
	wb_vector_release(&engine->container_uses);
	wb_arena_release(&engine->arena);
	free(engine);
}

/*
 * Adds an author style sheet (a <style> element's text), after the ones
 * added before it.
 */
int
css_engine_add_sheet(
	struct css_engine *engine,
	const uint16_t *units,
	size_t length)
{
	int error;

	/* An author sheet. */
	error = css_engine_add_sheet_origin(engine, units, length, CSS_ORIGIN_AUTHOR);
	if (error != 0)
		return error;

	/* Succeeded: the sheet takes part in the cascade. */
	return 0;
}

/*
 * Adds a style sheet of an origin.
 */
int
css_engine_add_sheet_origin(
	struct css_engine *engine,
	const uint16_t *units,
	size_t length,
	int origin)
{
	struct cascade_sheet entry;
	struct css_sheet *sheet;
	int error;

	/* Parses the sheet. */
	sheet = calloc(1, sizeof(*sheet));
	if (sheet == NULL)
		return ENOMEM;
	error = css_parse_sheet(engine->heap, units, length, origin, sheet);
	if (error != 0) {
		css_sheet_destroy(sheet);
		return error;
	}

	/* Appends it, owned by the engine, under no media list. */
	memset(&entry, 0, sizeof(entry));
	entry.sheet = sheet;
	entry.owned = sheet;
	error = wb_vector_push(&engine->sheets, &entry);
	if (error != 0) {
		css_sheet_destroy(sheet);
		return error;
	}

	/* Succeeded: the sheet is the engine's last. */
	return 0;
}

/*
 * Adds an author sheet the caller parsed (and keeps) after the ones added
 * before it, applying under every one of media_count media lists (NULL
 * ones hold); the engine only borrows the sheet and the lists.
 */
int
css_engine_add_parsed(
	struct css_engine *engine,
	const struct css_sheet *sheet,
	const struct css_media *const *media,
	size_t media_count)
{
	struct cascade_sheet entry;
	size_t index;
	int error;

	/* The lent sheet and every applicable media list. */
	memset(&entry, 0, sizeof(entry));
	entry.sheet = sheet;
	entry.owned = NULL;
	for (index = 0; index < media_count; index++) {
		if (media[index] == NULL)
			continue;
		if (entry.media_count == CSS_MEDIA_CHAIN_MAX)
			return E2BIG;
		entry.media[entry.media_count] = media[index];
		entry.media_count++;
	}

	/* Appends it. */
	error = wb_vector_push(&engine->sheets, &entry);
	if (error != 0)
		return error;

	/* Succeeded: the sheet takes part in the cascade. */
	return 0;
}

/*
 * Reads a media query list (a <link>'s media attribute) into the engine's
 * arena, for css_engine_add_parsed.
 */
int
css_engine_parse_media(
	struct css_engine *engine,
	const uint16_t *units,
	size_t length,
	const struct css_media **media)
{
	struct css_token *tokens;
	struct css_media *parsed;
	size_t count;
	int error;

	/* The attribute's tokens (without the end-of-file token). */
	error = css_tokenize(&engine->arena, units, length, &tokens, &count);
	if (error != 0)
		return error;
	if (count > 0)
		count--;

	/* The list. */
	error = css_media_parse(&engine->arena, tokens, count, NULL, &parsed);
	if (error != 0)
		return error;

	/* Succeeded: the list lives as long as the engine. */
	*media = parsed;
	return 0;
}

/*
 * Sets the viewport the vw and vh units measure.
 */
void
css_engine_set_viewport(
	struct css_engine *engine,
	float width,
	float height)
{
	/* The styles computed at another size may differ (vw, vh and the media queries). */
	if (engine->viewport_width != width || engine->viewport_height != height)
		cascade_style_forget(engine);

	/* Remembers the size. */
	engine->viewport_width = width;
	engine->viewport_height = height;
}

/*
 * Sets how the engine finds a query container's size for the
 * container-relative units; without one they are the
 * viewport's.
 */
void
css_engine_set_container_lookup(
	struct css_engine *engine,
	css_container_lookup lookup,
	void *context)
{
	/* The page's lookup. */
	engine->container_lookup = lookup;
	engine->container_context = context;
}

/*
 * Tells whether a container-relative unit had a query container whose size
 * the lookup did not know since the styles were last forgotten: the page
 * lays out again with the sizes it has now.
 */
int
css_engine_container_missed(
	const struct css_engine *engine)
{
	/* The count of misses, as a truth. */
	if (engine->container_missed != 0)
		return 1;

	/* Every container was known. */
	return 0;
}

/*
 * Forgets the computed styles the engine kept (they are
 * computed again with the query containers' sizes the page knows now).
 */
void
css_engine_forget_styles(
	struct css_engine *engine)
{
	/* The kept styles, the misses and the sizes used. */
	cascade_style_forget(engine);
	engine->container_missed = 0;
	wb_vector_clear(&engine->container_uses);
}

/*
 * Tells how many query containers' sizes the engine's styles were
 * computed with since the styles were last forgotten.
 */
size_t
css_engine_container_uses(
	const struct css_engine *engine)
{
	/* The number of containers used. */
	return engine->container_uses.count;
}

/*
 * Reports one query container and the size the engine's styles were
 * computed with.
 */
void
css_engine_container_use(
	const struct css_engine *engine,
	size_t index,
	const struct dom_element **container,
	float *width,
	float *height)
{
	const struct cascade_container_use *use;

	/* The recorded use. */
	use = wb_vector_at((struct wb_vector *)&engine->container_uses, index);
	*container = use->element;
	*width = use->width;
	*height = use->height;
}

/*
 * Computes an element's style from the cascade and its parent's style
 * (NULL for the root).
 */
int
css_engine_compute(
	struct css_engine *engine,
	struct dom_element *element,
	const struct css_style *parent,
	struct css_style *style)
{
	struct cascade_cached *cached;
	int error;

	/* A style the engine computed before for the element. */
	if (engine->style_capacity != 0) {
		cached = cascade_style_slot(engine, element);
		if (cached->element != NULL) {
			*style = *cached->style;
			return 0;
		}
	}

	/* The element itself. */
	error = cascade_compute(engine, element, parent, style, CSS_PSEUDO_ELEMENT_NONE);
	if (error != 0)
		return error;

	/* A second cascade supplies paint color without changing script-visible style or geometry. */
	error = cascade_paint_color(engine, element, parent, style, CSS_PSEUDO_ELEMENT_NONE);
	if (error != 0)
		return error;

	/* The engine keeps it (a copy it cannot keep is only computed again). */
	cascade_style_keep(engine, element, style);

	/* Succeeded: the style is computed. */
	return 0;
}

/*
 * Sets the page-local visited lookup used only for a paint-color cascade.
 * Script-visible selectors and computed styles still use the unvisited pass.
 */
void
css_engine_set_visited_lookup(
	struct css_engine *engine,
	int (*lookup)(void *, struct dom_element *),
	void *context)
{
	/* A changed lookup invalidates any previously computed styles. */
	engine->visited_lookup = lookup;
	engine->visited_context = context;
	cascade_style_forget(engine);
}

/*
 * Computes the style of an element's ::before or ::after (pseudo) from
 * the rules for it and the element's own style, which it inherits from.
 */
int
css_engine_compute_pseudo(
	struct css_engine *engine,
	struct dom_element *element,
	int pseudo,
	const struct css_style *element_style,
	struct css_style *style)
{
	int error;

	/* The pseudo-element's rules. */
	error = cascade_compute(engine, element, element_style, style, pseudo);
	if (error != 0)
		return error;

	/* A pseudo-element inherits the paint color of its owner, even if visited. */
	error = cascade_paint_color(engine, element, element_style, style, pseudo);
	if (error != 0)
		return error;

	/* Succeeded: the pseudo-element's style is computed. */
	return 0;
}

/* Computes only the alternate color and restores all cascade bookkeeping. */
static int
cascade_paint_color(
	struct css_engine *engine,
	struct dom_element *element,
	const struct css_style *parent,
	struct css_style *style,
	int pseudo)
{
	struct css_style visited;
	struct css_style visited_parent;
	const struct css_style *alternate_parent;
	float root_font_size;
	size_t container_uses;
	int container_missed;
	int error;

	/* With no observed history the ordinary cascade paints unchanged. */
	style->paint_color = style->color;
	if (engine->visited_lookup == NULL)
		return 0;

	/* Children inherit only the alternate color, not alternate dimensions or content. */
	alternate_parent = parent;
	if (parent != NULL) {
		visited_parent = *parent;
		visited_parent.color = parent->paint_color;
		alternate_parent = &visited_parent;
	}

	/* An alternate cascade is discarded except for color and cannot affect later layout. */
	root_font_size = engine->root_font_size;
	container_uses = engine->container_uses.count;
	container_missed = engine->container_missed;
	engine->visited_pass = 1;
	error = cascade_compute(engine, element, alternate_parent, &visited, pseudo);
	engine->visited_pass = 0;
	engine->root_font_size = root_font_size;
	engine->container_uses.count = container_uses;
	engine->container_missed = container_missed;
	if (error != 0)
		return error;
	style->paint_color = visited.color;
	return 0;
}

/* Computes the style of an element, or of one of its pseudo-elements, in the cascade. */
static int
cascade_compute(
	struct css_engine *engine,
	struct dom_element *element,
	const struct css_style *parent,
	struct css_style *style,
	int pseudo)
{
	struct cascade_match *matches;
	struct wb_vector list;
	struct wb_arena scratch;
	const struct css_declaration *font_size;
	const struct css_declaration *font_family;
	size_t index;
	int is_root;
	int error;
	int side;

	/* The element whose container-relative units are resolved. */
	engine->current = element;

	/* Starts from the initial values, with the inherited ones from the parent. */
	css_initial_style(style);
	if (parent != NULL) {
		style->color = parent->color;
		style->paint_color = parent->paint_color;
		style->font_size = parent->font_size;
		style->font_size_keyword = parent->font_size_keyword;
		style->font_weight = parent->font_weight;
		style->font_italic = parent->font_italic;
		style->generic_family = parent->generic_family;
		memcpy(style->families, parent->families, sizeof(style->families));
		style->family_count = parent->family_count;
		style->line_height = parent->line_height;
		style->text_align = parent->text_align;
		style->text_indent = parent->text_indent;
		style->direction = parent->direction;
		style->writing_mode = parent->writing_mode;
		style->writing_sideways = parent->writing_sideways;
		style->text_orientation = parent->text_orientation;
		style->border_spacing[0] = parent->border_spacing[0];
		style->border_spacing[1] = parent->border_spacing[1];
		style->border_collapse = parent->border_collapse;
		style->white_space = parent->white_space;
		style->text_transform = parent->text_transform;
		style->cursor = parent->cursor;
		style->visibility = parent->visibility;
		style->list_style = parent->list_style;
		style->underline = parent->underline;
		style->text_decoration_skip_ink = parent->text_decoration_skip_ink;
		style->text_shadow = parent->text_shadow;
		style->text_shadow_set = parent->text_shadow_set;
		style->custom = parent->custom;
	}

	/* Gathers the declarations that apply, in cascade order, noting the pseudo-elements some rule gives the element. */
	wb_vector_init(&list, sizeof(struct cascade_match));
	wb_arena_init(&scratch, 0);
	engine->pseudo_wanted = pseudo;
	engine->pseudo_seen = 0;
	error = cascade_collect(engine, element, &list, &scratch);
	engine->pseudo_wanted = CSS_PSEUDO_ELEMENT_NONE;
	if (pseudo == CSS_PSEUDO_ELEMENT_NONE)
		style->pseudo_elements = engine->pseudo_seen;
	if (error != 0) {
		wb_vector_release(&list);
		wb_arena_release(&scratch);
		return error;
	}

	/* Sorts them into cascade order (an empty list has no storage to sort). */
	matches = list.items;
	if (list.count > 1)
		qsort(matches, list.count, sizeof(*matches), cascade_compare);

	/* The element's custom properties, then the declarations that wait for them. */
	error = cascade_customs(engine, style, matches, list.count);
	if (error == 0)
		error = cascade_resolve_pending(engine, style, &list, &scratch);
	if (error != 0) {
		wb_vector_release(&list);
		wb_arena_release(&scratch);
		return error;
	}

	/* The list may have been rebuilt. */
	matches = list.items;

	/*
	 * The font size and family come first: the other lengths in em depend on
	 * the size, and the monospace family rescales a size on the keyword scale.
	 */
	font_size = NULL;
	font_family = NULL;
	for (index = 0; index < list.count; index++) {
		if (matches[index].declaration->property == CSS_PROP_FONT_SIZE)
			font_size = matches[index].declaration;
		if (matches[index].declaration->property == CSS_PROP_FONT_FAMILY)
			font_family = matches[index].declaration;
	}

	/* Applies the winning font size. */
	if (font_size != NULL)
		cascade_apply(engine, style, parent, font_size);

	/* Applies the winning font family. */
	if (font_family != NULL)
		cascade_apply(engine, style, parent, font_family);

	/* Rescales a keyword-scale size for a change to or from the monospace family. */
	cascade_monospace_size(style, parent, font_size);

	/* The final inherited flow determines which physical edge every logical margin uses. */
	for (index = 0; index < list.count; index++) {
		if (matches[index].declaration->property == CSS_PROP_WRITING_MODE ||
		    matches[index].declaration->property == CSS_PROP_DIRECTION)
			cascade_apply(engine, style, parent, matches[index].declaration);
	}

	/* Then every other declaration in order, the later winning. */
	for (index = 0; index < list.count; index++) {
		if (matches[index].declaration->property == CSS_PROP_FONT_SIZE)
			continue;
		if (matches[index].declaration->property == CSS_PROP_FONT_FAMILY)
			continue;
		if (matches[index].declaration->property == CSS_PROP_WRITING_MODE ||
		    matches[index].declaration->property == CSS_PROP_DIRECTION)
			continue;
		cascade_apply(engine, style, parent, matches[index].declaration);
	}

	/* The lists are no longer needed. */
	wb_vector_release(&list);
	wb_arena_release(&scratch);

	/* currentcolor in the border colors becomes the color. */
	for (side = 0; side < 4; side++) {
		if (style->border_color[side] == CSS_CURRENT_COLOR)
			style->border_color[side] = style->color;
		if (style->border_style[side] == CSS_BORDER_NONE)
			style->border_width[side] = 0;
	}

	/* And in the background color. */
	if (style->background_color == CSS_CURRENT_COLOR)
		style->background_color = style->color;

	/* And in the outline's color; an outline of no style has no width. */
	if (style->outline_color == CSS_CURRENT_COLOR)
		style->outline_color = style->color;
	if (style->outline_style == CSS_BORDER_NONE)
		style->outline_width = 0;

	/* And in the shadows' colors. */
	for (side = 0; side < style->shadow_count; side++) {
		if (style->shadows[side].color == CSS_CURRENT_COLOR)
			style->shadows[side].color = style->color;
	}
	if (style->text_shadow_set && style->text_shadow.color == CSS_CURRENT_COLOR)
		style->text_shadow.color = style->color;

	/* The root's font size is what rem measures (its pseudo-elements do not change it). */
	is_root = 0;
	if (pseudo == CSS_PSEUDO_ELEMENT_NONE && element->node.parent != NULL && element->node.parent->type == DOM_DOCUMENT)
		is_root = 1;
	if (is_root)
		engine->root_font_size = style->font_size;

	/* Succeeded: the style is computed. */
	return 0;
}

/*
 * Starts matching a script's selector lists (querySelector and the like) with an engine that outlives one styling.  The class
 * attributes split before are forgotten: a class a list names became an
 * atom only when the list was parsed, and a split made before would not
 * have it.
 */
void
css_engine_query_begin(
	struct css_engine *engine)
{
	/* No class attribute is split any more. */
	if (engine->class_table != NULL)
		memset(engine->class_table, 0, engine->class_capacity * sizeof(*engine->class_table));
	engine->class_count = 0;
	engine->class_atoms.count = 0;
}

/*
 * Tells whether an element matches any selector of a script's list (the
 * element itself: a selector of a pseudo-element matches nothing).
 */
int
css_engine_query_matches(
	struct css_engine *engine,
	struct dom_element *element,
	const struct css_query *query)
{
	const struct css_selector *selector;
	size_t index;
	int matches;

	/* Each selector of the list, until one matches. */
	for (index = 0; index < query->count; index++) {
		selector = &query->selectors[index];
		matches = cascade_selector_matches(engine, element, selector, selector->count - 1U, NULL);
		if (matches)
			return 1;
	}

	/* Succeeded: no selector matches. */
	return 0;
}

/*
 * Fills a style with the initial values of every property.
 */
void
css_initial_style(
	struct css_style *style)
{
	int side;

	/* Everything zero first, then the values that are not. */
	memset(style, 0, sizeof(*style));
	style->display = CSS_DISPLAY_INLINE;
	style->width.unit = CSS_UNIT_AUTO;
	style->height.unit = CSS_UNIT_AUTO;
	style->min_width.unit = CSS_UNIT_AUTO;
	style->max_width.unit = CSS_UNIT_NONE;
	style->min_height.unit = CSS_UNIT_AUTO;
	style->max_height.unit = CSS_UNIT_NONE;
	for (side = 0; side < 4; side++) {
		style->margin[side].unit = CSS_UNIT_PX;
		style->padding[side].unit = CSS_UNIT_PX;
		style->offset[side].unit = CSS_UNIT_AUTO;
		style->border_width[side] = 3;
		style->border_style[side] = CSS_BORDER_NONE;
		style->border_color[side] = CSS_CURRENT_COLOR;
	}

	/* The initial values that are not zero. */
	style->color = 0xff000000U;
	style->paint_color = style->color;
	style->background_color = 0;
	style->background_image = NULL;
	style->background_repeat = CSS_REPEAT_BOTH;
	style->background_attachment = CSS_BACKGROUND_SCROLL;
	style->transform_translate[0].unit = CSS_UNIT_PX;
	style->transform_translate[0].value = 0;
	style->transform_translate[0].offset = 0;
	style->transform_translate[1] = style->transform_translate[0];
	style->transform_scale[0].unit = CSS_UNIT_PX;
	style->transform_scale[0].value = 1.0f;
	style->transform_scale[0].offset = 0;
	style->transform_scale[1] = style->transform_scale[0];
	style->transform_origin[0].unit = CSS_UNIT_PERCENT;
	style->transform_origin[0].value = 50.0f;
	style->transform_origin[0].offset = 0;
	style->transform_origin[1] = style->transform_origin[0];
	style->aspect_ratio = 0.0f;
	style->aspect_auto = 1;
	style->object_fit = CSS_OBJECT_FIT_FILL;
	style->object_position[0].unit = CSS_UNIT_PERCENT;
	style->object_position[0].value = 50.0f;
	style->object_position[0].offset = 0;
	style->object_position[1] = style->object_position[0];
	style->background_position[0].unit = CSS_UNIT_PERCENT;
	style->background_position[0].value = 0;
	style->background_position[1].unit = CSS_UNIT_PERCENT;
	style->background_position[1].value = 0;
	style->background_size_keyword = CSS_BACKGROUND_SIZE_LENGTHS;
	style->background_size[0].unit = CSS_UNIT_AUTO;
	style->background_size[1].unit = CSS_UNIT_AUTO;
	style->font_size = CASCADE_DEFAULT_FONT_SIZE;
	style->font_size_keyword = 1;
	style->font_weight = 400;
	style->generic_family = CSS_FAMILY_SERIF;
	style->line_height.unit = CSS_UNIT_NORMAL;
	style->text_align = CSS_TEXT_ALIGN_START;
	style->white_space = CSS_WHITE_SPACE_NORMAL;
	style->text_transform = CSS_TEXT_TRANSFORM_NONE;
	style->cursor = CSS_CURSOR_AUTO;
	style->list_style = CSS_LIST_DISC;
	style->z_index_auto = 1;

	/* The flexible box's initial values that are not zero. */
	style->justify_content = CSS_ALIGN_NORMAL;
	style->justify_items = CSS_ALIGN_NORMAL;
	style->align_items = CSS_ALIGN_NORMAL;
	style->align_content = CSS_ALIGN_STRETCH;
	style->align_self = CSS_ALIGN_AUTO;
	style->justify_self = CSS_ALIGN_AUTO;
	style->flex_shrink = 1;
	style->flex_basis.unit = CSS_UNIT_AUTO;
	style->auto_column_count = 1;
	style->auto_columns[0].kind = CSS_TRACK_AUTO;
	style->auto_row_count = 1;
	style->auto_rows[0].kind = CSS_TRACK_AUTO;

	/* The decoration's initial values that are not zero: opaque, a medium outline of no style in currentcolor. */
	style->opacity = 1;
	style->outline_width = 3;
	style->outline_style = CSS_BORDER_NONE;
	style->outline_color = CSS_CURRENT_COLOR;
	style->text_decoration_current_color = 1;
}

/* Gathers every declaration that applies to an element: the matching rules' and the style attribute's. */
static int
cascade_collect(
	struct css_engine *engine,
	struct dom_element *element,
	struct wb_vector *matches,
	struct wb_arena *scratch)
{
	const struct cascade_sheet *sheets;
	struct css_declaration *declarations;
	struct dom_attribute *attribute;
	struct vm_string *inline_source;
	struct vm_string *id;
	struct wb_units units;
	size_t declaration_count;
	size_t sheet;
	size_t index;
	int holds;
	int error;

	/* HTML's presentational hints precede the author's sheets at specificity zero. */
	if (engine->pseudo_wanted == CSS_PSEUDO_ELEMENT_NONE) {
		error = cascade_presentational(engine, element, matches, scratch);
		if (error != 0)
			return error;
	}

	/* The keys the rule indexes are searched by: the element's id, classes and type. */
	error = cascade_element_keys(engine, element, &id);
	if (error != 0)
		return error;

	/* The rules of every sheet whose media hold and whose selectors match, in the order of the sheets. */
	sheets = engine->sheets.items;
	for (sheet = 0; sheet < engine->sheets.count; sheet++) {
		holds = 1;
		for (index = 0; index < sheets[sheet].media_count && holds; index++)
			holds = css_media_matches(sheets[sheet].media[index], engine->viewport_width, engine->viewport_height);
		if (!holds)
			continue;

		/* The sheet's matching rules. */
		error = cascade_collect_sheet(engine, element, sheets[sheet].sheet, (uint64_t)sheet, id, matches);
		if (error != 0)
			return error;
	}

	/* The style attribute, more specific than any selector (the element's own, not its pseudo-elements'). */
	if (engine->pseudo_wanted != CSS_PSEUDO_ELEMENT_NONE)
		return 0;
	inline_source = element->inline_style_source;
	if (inline_source == NULL) {
		attribute = dom_element_find_attribute(element, DOM_NS_NONE, engine->atom_style);
		if (attribute == NULL)
			return 0;
		inline_source = attribute->value;
	}

	/* The native declaration source retains precision that public serialized text may round. */
	wb_units_init(&units);
	error = vm_string_append_units(inline_source, &units);

	/* Parses the attribute's declarations. */
	if (error == 0)
		error = css_parse_declarations(engine->heap, scratch, units.data, units.length, &declarations, &declaration_count);
	wb_units_release(&units);
	if (error == ENOMEM)
		return error;
	if (error != 0)
		return 0;

	/* They come after every sheet's rules. */
	error = cascade_add_declarations(matches, declarations, declaration_count, CSS_ORIGIN_AUTHOR, 0xffffffffU, (uint64_t)engine->sheets.count << 44);
	if (error != 0)
		return error;

	/* Succeeded: every applying declaration is gathered. */
	return 0;
}

/*
 * Adds the common HTML presentational hints as author declarations of
 * specificity zero.  They therefore win over the user agent sheet and lose
 * to an author's rule, including a universal selector later in the source.
 */
static int
cascade_presentational(
	struct css_engine *engine,
	struct dom_element *element,
	struct wb_vector *matches,
	struct wb_arena *scratch)
{
	struct css_declaration *declarations;
	const struct vm_string *value;
	struct wb_units units;
	size_t declaration_count;
	int table;
	int cell;
	int row;
	int row_group;
	int text_align;
	int font;
	int error;

	/* Classifies the elements that accept each old HTML attribute. */
	table = dom_element_is(&element->node, DOM_NS_HTML, DOM_TAG_TABLE);
	cell = dom_element_is(&element->node, DOM_NS_HTML, DOM_TAG_TD) ||
	    dom_element_is(&element->node, DOM_NS_HTML, DOM_TAG_TH);
	row = dom_element_is(&element->node, DOM_NS_HTML, DOM_TAG_TR);
	row_group = dom_element_is(&element->node, DOM_NS_HTML, DOM_TAG_THEAD) ||
	    dom_element_is(&element->node, DOM_NS_HTML, DOM_TAG_TBODY) ||
	    dom_element_is(&element->node, DOM_NS_HTML, DOM_TAG_TFOOT);
	text_align = cell || row ||
	    dom_element_is(&element->node, DOM_NS_HTML, DOM_TAG_DIV) ||
	    dom_element_is(&element->node, DOM_NS_HTML, DOM_TAG_P) ||
	    dom_element_is(&element->node, DOM_NS_HTML, DOM_TAG_H1) ||
	    dom_element_is(&element->node, DOM_NS_HTML, DOM_TAG_H2) ||
	    dom_element_is(&element->node, DOM_NS_HTML, DOM_TAG_H3) ||
	    dom_element_is(&element->node, DOM_NS_HTML, DOM_TAG_H4) ||
	    dom_element_is(&element->node, DOM_NS_HTML, DOM_TAG_H5) ||
	    dom_element_is(&element->node, DOM_NS_HTML, DOM_TAG_H6);
	font = dom_element_is(&element->node, DOM_NS_HTML, DOM_TAG_FONT);

	/* Width applies to tables and cells. */
	wb_units_init(&units);
	error = 0;
	if (table || cell) {
		value = dom_attribute_ascii(element, "width");
		if (value != NULL)
			error = cascade_hint_length(&units, "width", value);
	}

	/* Height applies to tables, rows and cells. */
	if (error == 0 && (table || cell || row)) {
		value = dom_attribute_ascii(element, "height");
		if (value != NULL)
			error = cascade_hint_length(&units, "height", value);
	}

	/* cellspacing becomes the table's border spacing. */
	if (error == 0 && table) {
		value = dom_attribute_ascii(element, "cellspacing");
		if (value != NULL)
			error = cascade_hint_length(&units, "border-spacing", value);
	}

	/* A table's align moves the table; the other old align attributes align text. */
	value = dom_attribute_ascii(element, "align");
	if (error == 0 && table && value != NULL)
		error = cascade_hint_keywords(&units, value, cascade_table_align);
	else if (error == 0 && text_align && value != NULL)
		error = cascade_hint_keywords(&units, value, cascade_text_align);

	/* Table groups, rows and cells accept their legacy vertical alignment. */
	value = dom_attribute_ascii(element, "valign");
	if (error == 0 && (cell || row || row_group) && value != NULL)
		error = cascade_hint_keywords(&units, value, cascade_vertical_align);

	/* Legacy table colors and font colors are still used by simple pages. */
	value = dom_attribute_ascii(element, "bgcolor");
	if (error == 0 && (table || cell || row || row_group) && value != NULL)
		error = cascade_hint_color(&units, "background-color", value);
	value = dom_attribute_ascii(element, "color");
	if (error == 0 && font && value != NULL)
		error = cascade_hint_color(&units, "color", value);

	/* A cell's bare nowrap attribute preserves its spaces and lines. */
	value = dom_attribute_ascii(element, "nowrap");
	if (error == 0 && cell && value != NULL)
		error = cascade_hint_ascii(&units, "white-space:nowrap;");
	if (error != 0) {
		wb_units_release(&units);
		return error;
	}

	/* An element without a recognized hint contributes no declaration. */
	if (units.length == 0) {
		wb_units_release(&units);
		return 0;
	}

	/* Parse the sanitized declarations in the same scratch arena as style=. */
	error = css_parse_declarations(engine->heap, scratch, units.data,
	    units.length, &declarations, &declaration_count);
	wb_units_release(&units);
	if (error == ENOMEM)
		return error;
	if (error != 0)
		return 0;
	return cascade_add_declarations(matches, declarations, declaration_count,
	    CSS_ORIGIN_AUTHOR, 0, 0);
}

/* Appends an ASCII fragment to a declaration being made. */
static int
cascade_hint_ascii(
	struct wb_units *units,
	const char *text)
{
	size_t index;
	int error;

	/* Copies the fragment one ASCII character at a time. */
	error = 0;
	for (index = 0; text[index] != '\0' && error == 0; index++)
		error = wb_units_append_code_point(units, (unsigned char)text[index]);
	return error;
}

/* Appends a nonnegative integer HTML dimension as px or a percentage. */
static int
cascade_hint_length(
	struct wb_units *units,
	const char *property,
	const struct vm_string *value)
{
	size_t first;
	size_t last;
	size_t index;
	uint16_t unit;
	int percent;
	int error;

	/* Skips the leading ASCII whitespace. */
	index = 0;
	while (index < value->length) {
		unit = vm_string_at(value, index);
		if (unit != ' ' && unit != '\t' && unit != '\n' && unit != '\r' && unit != '\f')
			break;
		index++;
	}

	/* Takes the run of decimal digits. */
	first = index;
	while (index < value->length) {
		unit = vm_string_at(value, index);
		if (unit < '0' || unit > '9')
			break;
		index++;
	}

	/* A dimension without a number is not a hint. */
	last = index;
	if (last == first)
		return 0;

	/* Skips spaces between the number and an optional percent sign. */
	while (index < value->length) {
		unit = vm_string_at(value, index);
		if (unit != ' ')
			break;
		index++;
	}

	/* Takes the percent sign when one is present. */
	percent = 0;
	if (index < value->length) {
		unit = vm_string_at(value, index);
		if (unit == '%') {
			percent = 1;
			index++;
		}
	}

	/* Only spaces may follow the value. */
	while (index < value->length) {
		unit = vm_string_at(value, index);
		if (unit != ' ')
			break;
		index++;
	}

	/* Rejects anything other than the parsed dimension. */
	if (index != value->length)
		return 0;

	/* Writes the property, the sanitized number and its unit. */
	error = cascade_hint_ascii(units, property);
	if (error == 0)
		error = cascade_hint_ascii(units, ":");
	for (index = first; index < last && error == 0; index++)
		error = wb_units_append_code_point(units, vm_string_at(value, index));
	if (error == 0 && percent)
		error = cascade_hint_ascii(units, "%;");
	if (error == 0 && !percent)
		error = cascade_hint_ascii(units, "px;");

	/* Reports whether the declaration was written. */
	return error;
}

/* Appends a legacy named or hexadecimal color, excluding CSS punctuation. */
static int
cascade_hint_color(
	struct wb_units *units,
	const char *property,
	const struct vm_string *value)
{
	size_t first;
	size_t last;
	size_t index;
	uint16_t unit;
	int allowed;
	int error;

	/* Trims spaces around the color. */
	first = 0;
	while (first < value->length) {
		unit = vm_string_at(value, first);
		if (unit != ' ')
			break;
		first++;
	}

	/* Trims the trailing spaces. */
	last = value->length;
	while (last > first) {
		unit = vm_string_at(value, last - 1U);
		if (unit != ' ')
			break;
		last--;
	}

	/* An empty value is not a color hint. */
	if (first == last)
		return 0;

	/* Rejects punctuation which could add another declaration. */
	for (index = first; index < last; index++) {
		unit = vm_string_at(value, index);
		allowed = unit == '#' || (unit >= '0' && unit <= '9') ||
		    (unit >= 'A' && unit <= 'Z') || (unit >= 'a' && unit <= 'z');
		if (!allowed)
			return 0;
	}

	/* Writes the safe color declaration. */
	error = cascade_hint_ascii(units, property);
	if (error == 0)
		error = cascade_hint_ascii(units, ":");
	for (index = first; index < last && error == 0; index++)
		error = wb_units_append_code_point(units, vm_string_at(value, index));
	if (error == 0)
		error = cascade_hint_ascii(units, ";");

	/* Reports whether the declaration was written. */
	return error;
}

/* Compares a legacy keyword after trimming ASCII whitespace, ignoring case. */
static int
cascade_hint_equal(
	const struct vm_string *value,
	const char *keyword)
{
	size_t first;
	size_t last;
	size_t index;
	uint16_t unit;
	unsigned char expected;

	/* Trims spaces around the old keyword. */
	first = 0;
	while (first < value->length) {
		unit = vm_string_at(value, first);
		if (unit != ' ')
			break;
		first++;
	}

	/* Trims the trailing spaces. */
	last = value->length;
	while (last > first) {
		unit = vm_string_at(value, last - 1U);
		if (unit != ' ')
			break;
		last--;
	}

	/* Compares its ASCII letters without case. */
	for (index = 0; first + index < last && keyword[index] != '\0'; index++) {
		unit = vm_string_at(value, first + index);
		expected = (unsigned char)keyword[index];
		if (unit >= 'A' && unit <= 'Z')
			unit = (uint16_t)(unit - 'A' + 'a');
		if (unit != expected)
			return 0;
	}

	/* Both strings must end together. */
	return first + index == last && keyword[index] == '\0';
}

/* Appends the declaration of a recognized legacy keyword. */
static int
cascade_hint_keywords(
	struct wb_units *units,
	const struct vm_string *value,
	const struct cascade_hint_keyword *keywords)
{
	size_t index;
	int same;

	/* Uses the first matching keyword from the small fixed table. */
	for (index = 0; keywords[index].keyword != NULL; index++) {
		same = cascade_hint_equal(value, keywords[index].keyword);
		if (same)
			return cascade_hint_ascii(units, keywords[index].declaration);
	}

	/* An unknown value contributes no declaration. */
	return 0;
}

/*
 * Finds the atoms an element is looked up by in the rule indexes: its id
 * (NULL when it has none, or when no sheet names it), and its classes in
 * the engine's class_keys (the ones no sheet names are left out: no atom
 * exists for them, so no selector can name them).
 */
static int
cascade_element_keys(
	struct css_engine *engine,
	struct dom_element *element,
	struct vm_string **id)
{
	struct dom_attribute *attribute;
	struct vm_string *const *atoms;
	size_t count;
	size_t position;
	uint16_t unit;
	int error;

	/* The id's atom, found without making one. */
	*id = NULL;
	wb_vector_clear(&engine->class_keys);
	attribute = dom_element_find_attribute(element, DOM_NS_NONE, engine->atom_id);
	if (attribute != NULL) {
		wb_units_clear(&engine->word);
		for (position = 0; position < attribute->value->length; position++) {
			unit = vm_string_at(attribute->value, position);
			error = wb_units_append(&engine->word, &unit, 1);
			if (error != 0)
				return ENOMEM;
		}

		/* The atom, when a sheet made one. */
		*id = vm_atom_find_units(engine->heap, engine->word.data, engine->word.length);
	}

	/* An element without classes has no class keys. */
	attribute = dom_element_find_attribute(element, DOM_NS_NONE, engine->atom_class);
	if (attribute == NULL)
		return 0;

	/* The class attribute's words that some sheet named (their atoms, kept by the engine). */
	error = cascade_class_atoms(engine, attribute->value, &atoms, &count);
	if (error != 0)
		return error;

	/* Each is a key. */
	for (position = 0; position < count; position++) {
		error = wb_vector_push(&engine->class_keys, &atoms[position]);
		if (error != 0)
			return ENOMEM;
	}

	/* Succeeded: the element's keys are found. */
	return 0;
}

/* Appends the index entries of a bucket (NULL: none) to the engine's candidates. */
static int
cascade_add_bucket(
	struct css_engine *engine,
	const struct css_rule_index *index,
	const struct css_index_bucket *bucket)
{
	uint32_t position;
	int error;

	/* A key no selector is filed under adds nothing. */
	if (bucket == NULL)
		return 0;

	/* Each entry of the run. */
	for (position = bucket->start; position < bucket->start + bucket->count; position++) {
		error = wb_vector_push(&engine->candidates, &index->entries[position]);
		if (error != 0)
			return ENOMEM;
	}

	/* Succeeded: the run is among the candidates. */
	return 0;
}

/*
 * Gathers the declarations of one sheet's rules that match an element:
 * the selectors its index files under the element's id, classes and type
 * and the universal ones, tried in rule order, each rule once with the
 * most specific of its selectors that match.
 */
static int
cascade_collect_sheet(
	struct css_engine *engine,
	struct dom_element *element,
	const struct css_sheet *sheet,
	uint64_t sheet_number,
	struct vm_string *id,
	struct wb_vector *matches)
{
	const struct css_rule_index *index;
	const struct css_index_bucket *bucket;
	const struct css_index_entry *candidates;
	const struct css_rule *rule;
	const struct css_selector *selector;
	struct css_index_bucket universal;
	struct vm_string **class_keys;
	uint32_t best;
	uint64_t order;
	size_t position;
	size_t group;
	size_t item;
	int matched;
	int holds;
	int selector_matched;
	int error;

	/* The candidates: the runs of the id, each class, the type, and the universal run. */
	index = &sheet->index;
	wb_vector_clear(&engine->candidates);
	bucket = css_index_find(&index->ids, id);
	error = cascade_add_bucket(engine, index, bucket);
	class_keys = engine->class_keys.items;
	for (position = 0; position < engine->class_keys.count && error == 0; position++) {
		bucket = css_index_find(&index->classes, class_keys[position]);
		error = cascade_add_bucket(engine, index, bucket);
	}

	/* The type's run. */
	if (error == 0) {
		bucket = css_index_find(&index->tags, element->local_name);
		error = cascade_add_bucket(engine, index, bucket);
	}

	/* The universal run. */
	if (error == 0) {
		universal.key = NULL;
		universal.start = index->universal_start;
		universal.count = index->universal_count;
		error = cascade_add_bucket(engine, index, &universal);
	}

	/* Memory ran out on the way. */
	if (error != 0)
		return error;

	/* Several runs are merged into rule order (each run is in rule order already). */
	candidates = engine->candidates.items;
	if (engine->candidates.count > 1)
		qsort(engine->candidates.items, engine->candidates.count, sizeof(struct css_index_entry), cascade_entry_compare);

	/* Tries the candidates rule by rule. */
	position = 0;
	while (position < engine->candidates.count) {
		rule = &sheet->rules[candidates[position].rule];
		matched = 0;
		best = 0;

		/* The end of the rule's candidates. */
		group = position;
		while (group < engine->candidates.count && candidates[group].rule == candidates[position].rule)
			group++;

		/* A rule inside @media applies only while its lists hold. */
		holds = 1;
		if (rule->media != NULL)
			holds = css_media_matches(rule->media, engine->viewport_width, engine->viewport_height);

		/* Every candidate selector of the rule. */
		for (item = position; item < group && holds; item++) {
			selector = &rule->selectors[candidates[item].selector];
			selector_matched = cascade_candidate_matches(engine, element, selector);
			if (selector_matched) {
				if (!matched || selector->specificity > best)
					best = selector->specificity;
				matched = 1;
			}
		}

		/* A rule whose selectors all failed does not apply. */
		if (matched) {
			order = (sheet_number << 44) | ((uint64_t)candidates[position].rule << 16);
			error = cascade_add_declarations(matches, rule->declarations, rule->declaration_count, sheet->origin, best, order);
			if (error != 0)
				return error;
		}

		/* On to the next rule. */
		position = group;
	}

	/* Succeeded: the sheet's applying declarations are gathered. */
	return 0;
}

/* Orders two index entries by rule, then selector. */
static int
cascade_entry_compare(
	const void *left,
	const void *right)
{
	const struct css_index_entry *a;
	const struct css_index_entry *b;

	/* Compares the rule, then the selector. */
	a = left;
	b = right;
	if (a->rule < b->rule)
		return -1;
	if (a->rule > b->rule)
		return 1;
	if (a->selector < b->selector)
		return -1;
	if (a->selector > b->selector)
		return 1;

	/* The same entry. */
	return 0;
}

/* Adds a rule's declarations with their origin, specificity and order. */
static int
cascade_add_declarations(
	struct wb_vector *matches,
	const struct css_declaration *declarations,
	size_t count,
	int origin,
	uint32_t specificity,
	uint64_t order)
{
	struct cascade_match match;
	size_t index;
	int error;

	/* Each declaration with its rank. */
	for (index = 0; index < count; index++) {
		match.declaration = &declarations[index];
		match.specificity = specificity;
		match.order = order + (uint64_t)index;
		if (origin == CSS_ORIGIN_USER_AGENT) {
			match.rank = RANK_USER_AGENT;
			if (declarations[index].important)
				match.rank = RANK_USER_AGENT_IMPORTANT;
		} else {
			match.rank = RANK_AUTHOR;
			if (declarations[index].important)
				match.rank = RANK_AUTHOR_IMPORTANT;
		}

		/* Adds it. */
		error = wb_vector_push(matches, &match);
		if (error != 0)
			return ENOMEM;
	}

	/* Succeeded: the declarations are gathered. */
	return 0;
}

/*
 * Gives the element its custom properties: its parent's list, with the
 * ones it declares in front (the last declaration of a name found first),
 * each with its var() references replaced; a property whose references
 * cannot be replaced is invalid.  The list lives in the engine's arena.
 */
static int
cascade_customs(
	struct css_engine *engine,
	struct css_style *style,
	const struct cascade_match *matches,
	size_t count)
{
	struct css_custom *node;
	struct css_custom **own;
	struct wb_vector owned;
	struct wb_vector tokens;
	size_t index;
	int error;

	/* The element's own properties go in front of the inherited list, in cascade order. */
	wb_vector_init(&owned, sizeof(struct css_custom *));
	for (index = 0; index < count; index++) {
		if (matches[index].declaration->property != CSS_PROP_CUSTOM)
			continue;

		/* A node for the declaration, its tokens as written for now. */
		node = wb_arena_zalloc(&engine->arena, sizeof(*node));
		if (node == NULL) {
			wb_vector_release(&owned);
			return ENOMEM;
		}

		/* The node is the list's first now. */
		node->name = matches[index].declaration->custom_name;
		node->tokens = matches[index].declaration->raw;
		node->count = matches[index].declaration->raw_count;
		node->next = style->custom;
		style->custom = node;
		error = wb_vector_push(&owned, &node);
		if (error != 0) {
			wb_vector_release(&owned);
			return ENOMEM;
		}
	}

	/* An element that declares none shares its parent's list. */
	if (owned.count == 0) {
		wb_vector_release(&owned);
		return 0;
	}

	/* Each own property's tokens, with their references replaced, copied into the engine's arena. */
	own = owned.items;
	wb_vector_init(&tokens, sizeof(struct css_token));
	for (index = 0; index < owned.count; index++) {
		node = own[index];
		wb_vector_clear(&tokens);
		error = cascade_substitute(style->custom, node->tokens, node->count, 0, &tokens);
		if (error == ENOMEM) {
			wb_vector_release(&tokens);
			wb_vector_release(&owned);
			return error;
		}

		/* A reference that cannot be replaced makes the property invalid. */
		if (error != 0) {
			node->invalid = 1;
			node->tokens = NULL;
			node->count = 0;
			continue;
		}

		/* The replaced tokens, kept as long as the engine. */
		error = cascade_keep_tokens(engine, tokens.items, tokens.count, &node->tokens);
		if (error != 0) {
			wb_vector_release(&tokens);
			wb_vector_release(&owned);
			return error;
		}

		/* The node holds the replaced tokens. */
		node->count = tokens.count;
	}

	/* The buffers are no longer needed. */
	wb_vector_release(&tokens);
	wb_vector_release(&owned);

	/* Succeeded: the element has its custom properties. */
	return 0;
}

/*
 * Appends tokens to a buffer with every var(--name[, fallback]) replaced
 * by the named property's tokens from a custom property list (its own
 * references replaced in turn, up to a depth that stops a cycle), or by
 * the fallback.  Returns EINVAL when a reference has neither.
 */
static int
cascade_substitute(
	const struct css_custom *list,
	const struct css_token *tokens,
	size_t count,
	int depth,
	struct wb_vector *out)
{
	const struct css_custom *node;
	size_t index;
	size_t end;
	size_t name_index;
	size_t fallback;
	int nesting;
	int is_var;
	int error;

	/* A reference too deep is a cycle. */
	if (depth > CASCADE_VAR_DEPTH)
		return EINVAL;

	/* Copies token by token, replacing each var(). */
	index = 0;
	while (index < count) {
		is_var = 0;
		if (tokens[index].type == CSS_TOKEN_FUNCTION)
			is_var = css_ident_equal(&tokens[index], "var");
		if (!is_var) {
			error = wb_vector_push(out, &tokens[index]);
			if (error != 0)
				return ENOMEM;
			index++;
			continue;
		}

		/* The reference's closing parenthesis. */
		end = index + 1U;
		nesting = 1;
		while (end < count) {
			if (tokens[end].type == CSS_TOKEN_FUNCTION || tokens[end].type == CSS_TOKEN_OPEN_PAREN)
				nesting++;
			if (tokens[end].type == CSS_TOKEN_CLOSE_PAREN)
				nesting--;
			if (nesting == 0)
				break;
			end++;
		}

		/* The name, and the fallback after a comma. */
		name_index = index + 1U;
		while (name_index < end && tokens[name_index].type == CSS_TOKEN_WHITESPACE)
			name_index++;
		if (name_index >= end || tokens[name_index].type != CSS_TOKEN_IDENT)
			return EINVAL;
		fallback = name_index + 1U;
		while (fallback < end && tokens[fallback].type == CSS_TOKEN_WHITESPACE)
			fallback++;
		if (fallback < end && tokens[fallback].type != CSS_TOKEN_COMMA)
			return EINVAL;

		/* The named property, when the list has a valid one. */
		node = cascade_custom_find(list, &tokens[name_index]);
		if (node != NULL) {
			error = cascade_substitute(list, node->tokens, node->count, depth + 1, out);
		} else if (fallback < end) {
			error = cascade_substitute(list, tokens + fallback + 1U, end - fallback - 1U, depth + 1, out);
		} else {
			error = EINVAL;
		}

		/* A reference that could not be replaced spoils the value. */
		if (error != 0)
			return error;

		/* Past the reference. */
		index = end + 1U;
	}

	/* Succeeded: the tokens are appended. */
	return 0;
}

/* Finds the valid custom property a name token names in a list (NULL when it has none). */
static const struct css_custom *
cascade_custom_find(
	const struct css_custom *list,
	const struct css_token *name)
{
	const struct css_custom *node;
	int same;

	/* The first node of the name is the one that wins. */
	for (node = list; node != NULL; node = node->next) {
		same = vm_string_equal_units(node->name, name->text, name->length);
		if (!same)
			continue;

		/* An invalid property is as good as none. */
		if (node->invalid)
			return NULL;
		return node;
	}

	/* No property of the name. */
	return NULL;
}

/* Copies tokens and their text into the engine's arena. */
static int
cascade_keep_tokens(
	struct css_engine *engine,
	const struct css_token *tokens,
	size_t count,
	const struct css_token **kept)
{
	struct css_token *copy;
	uint16_t *text;
	size_t index;

	/* An empty value keeps no tokens. */
	*kept = NULL;
	if (count == 0)
		return 0;

	/* The tokens. */
	copy = wb_arena_alloc(&engine->arena, count * sizeof(*copy));
	if (copy == NULL)
		return ENOMEM;
	memcpy(copy, tokens, count * sizeof(*copy));

	/* Each token's text, which may live in an arena that ends sooner. */
	for (index = 0; index < count; index++) {
		if (copy[index].text == NULL || copy[index].length == 0)
			continue;
		text = wb_arena_alloc(&engine->arena, copy[index].length * sizeof(uint16_t));
		if (text == NULL)
			return ENOMEM;
		memcpy(text, copy[index].text, copy[index].length * sizeof(uint16_t));
		copy[index].text = text;
	}

	/* Succeeded: the tokens live as long as the engine. */
	*kept = copy;
	return 0;
}

/*
 * Replaces each declaration that waits for var() in a sorted list of
 * matches by the declarations its value makes once the element's custom
 * properties are substituted (in the scratch arena), in the same place of
 * the cascade; a value that cannot be substituted or parsed makes the
 * property unset.
 */
static int
cascade_resolve_pending(
	struct css_engine *engine,
	const struct css_style *style,
	struct wb_vector *list,
	struct wb_arena *scratch)
{
	struct css_declaration expanded[CASCADE_EXPANSION_MAX];
	struct css_declaration *kept;
	struct cascade_match *matches;
	struct cascade_match match;
	struct css_parse parse;
	struct css_token unset;
	struct wb_vector tokens;
	struct wb_vector resolved;
	static const uint16_t unset_text[] = { 'u', 'n', 's', 'e', 't' };
	size_t index;
	size_t item;
	size_t made;
	int pending;
	int error;

	/* Nothing to do without a pending declaration. */
	matches = list->items;
	pending = 0;
	for (index = 0; index < list->count && !pending; index++) {
		if (matches[index].declaration->property == CSS_PROP_PENDING)
			pending = 1;
	}

	/* A list without one stays as it is. */
	if (!pending)
		return 0;

	/* The value parser's context: the heap's names, calculations in the scratch arena. */
	parse.heap = engine->heap;
	parse.arena = scratch;
	memset(&unset, 0, sizeof(unset));
	unset.type = CSS_TOKEN_IDENT;
	unset.text = unset_text;
	unset.length = 5;

	/* Rebuilds the list with each pending declaration expanded in its place. */
	wb_vector_init(&tokens, sizeof(struct css_token));
	wb_vector_init(&resolved, sizeof(struct cascade_match));
	error = 0;
	for (index = 0; index < list->count && error == 0; index++) {
		if (matches[index].declaration->property != CSS_PROP_PENDING) {
			error = wb_vector_push(&resolved, &matches[index]);
			continue;
		}

		/* The value with the element's custom properties, parsed as its property. */
		wb_vector_clear(&tokens);
		made = 0;
		error = cascade_substitute(style->custom, matches[index].declaration->raw, matches[index].declaration->raw_count, 0, &tokens);
		if (error == 0)
			error = css_parse_property(&parse, matches[index].declaration->pending_property, tokens.items, tokens.count, expanded, &made);
		if (error == ENOMEM)
			break;

		/* A value invalid at computed-value time is unset. */
		if (error != 0 || made == 0) {
			made = 0;
			error = css_parse_property(&parse, matches[index].declaration->pending_property, &unset, 1, expanded, &made);
			if (error != 0)
				break;
		}

		/* The declarations made, kept in the scratch arena, in the pending one's place. */
		kept = wb_arena_alloc(scratch, made * sizeof(*kept) + 1U);
		if (kept == NULL) {
			error = ENOMEM;
			break;
		}

		/* Each one takes the pending declaration's rank and order. */
		memcpy(kept, expanded, made * sizeof(*kept));
		for (item = 0; item < made && error == 0; item++) {
			match = matches[index];
			match.declaration = &kept[item];
			error = wb_vector_push(&resolved, &match);
		}
	}

	/* The substituted tokens are no longer needed. */
	wb_vector_release(&tokens);
	if (error != 0) {
		wb_vector_release(&resolved);
		return error;
	}

	/* The resolved list replaces the old one. */
	wb_vector_release(list);
	*list = resolved;

	/* Succeeded: no declaration waits any more. */
	return 0;
}

/*
 * Computes a calculated length: pixels, or a percentage with pixels added
 * (min(), max() and clamp() over percentages measure them against the
 * viewport's width, as the layout's containing block is not known here).
 */
static struct css_length
cascade_calc(
	struct css_engine *engine,
	const struct css_calc *calc,
	float font_size)
{
	struct css_length length;
	float values[CSS_CALC_ARGUMENTS];
	float percents[CSS_CALC_ARGUMENTS];
	float container_width;
	float container_height;
	float least;
	float nested_pixels;
	float vmin;
	float vmax;
	size_t index;
	int any_percent;

	/* Each argument's pixels and percentage (a calculation has one argument at least). */
	memset(values, 0, sizeof(values));
	memset(percents, 0, sizeof(percents));

	/* Chooses viewport axes for the relative length components. */
	vmin = engine->viewport_width;
	if (engine->viewport_height < vmin)
		vmin = engine->viewport_height;

	/* Chooses the larger viewport axis independently. */
	vmax = engine->viewport_width;
	if (engine->viewport_height > vmax)
		vmax = engine->viewport_height;

	/* Computes each argument before choosing the outer operation. */
	any_percent = 0;
	for (index = 0; index < calc->count; index++) {
		values[index] = calc->sums[index].px +
				calc->sums[index].em * font_size +
				calc->sums[index].ex * font_size * 0.5f +
				calc->sums[index].rem * engine->root_font_size +
				calc->sums[index].vw * engine->viewport_width / 100.0f +
				calc->sums[index].vh * engine->viewport_height / 100.0f +
				calc->sums[index].vmin * vmin / 100.0f +
				calc->sums[index].vmax * vmax / 100.0f;

		/* Resolves container-relative terms only when the expression needs them. */
		if (calc->sums[index].cqw != 0 || calc->sums[index].cqh != 0) {
			cascade_container(engine, &container_width, &container_height);
			values[index] += calc->sums[index].cqw * container_width / 100.0f + calc->sums[index].cqh * container_height / 100.0f;
		}

		/* A min(), max() or clamp() inside the sum. */
		if (calc->sums[index].nested != NULL) {
			nested_pixels = cascade_calc_pixels(engine, calc->sums[index].nested, font_size);
			values[index] += calc->sums[index].nested_factor * nested_pixels;
		}

		/* Records the percentage coefficient for deferred or nonlinear resolution. */
		percents[index] = calc->sums[index].percent;
		if (percents[index] != 0)
			any_percent = 1;
	}

	/* A sum keeps its percentage for the layout. */
	length.value = values[0];
	length.unit = CSS_UNIT_PX;
	length.offset = 0;

	/* A linear sum can retain its containing-block dependency. */
	if (calc->operation == CSS_CALC_SUM) {
		/* Direct linear sums preserve percentage type even when arithmetic cancels its coefficient. */
		if (percents[0] != 0 ||
		    (calc->sums[0].has_percent && calc->sums[0].nested == NULL)) {
			length.unit = CSS_UNIT_PERCENT;
			length.value = percents[0];
			length.offset = values[0];
		}

		/* Succeeded: the linear length retains its computed type. */
		return length;
	}

	/* The others compare pixels: a percentage is measured against the viewport's width. */
	if (any_percent) {
		/* Resolves each nonlinear argument using the existing viewport approximation. */
		for (index = 0; index < calc->count; index++)
			values[index] += percents[index] * engine->viewport_width / 100.0f;
	}

	/* min() takes the least, max() the greatest. */
	least = values[0];
	for (index = 1; index < calc->count; index++) {
		/* A minimum selects the lowest resolved argument. */
		if (calc->operation == CSS_CALC_MIN && values[index] < least)
			least = values[index];

		/* The other nonlinear operations first select the greatest argument. */
		if (calc->operation != CSS_CALC_MIN && values[index] > least)
			least = values[index];
	}

	/* clamp(low, preferred, high) is the preferred one held between the two. */
	if (calc->operation == CSS_CALC_CLAMP) {
		least = values[1];

		/* The upper bound limits the preferred length first. */
		if (least > values[2])
			least = values[2];

		/* The lower bound wins when the range is reversed. */
		if (least < values[0])
			least = values[0];
	}

	/* Succeeded: the nonlinear expression resolves to its existing pixel approximation. */
	length.value = least;
	return length;
}

/*
 * Finds the size the container-relative units of the element being styled
 * are measured against: the content box of its nearest
 * ancestor whose style (kept by the engine) makes it a query container, as
 * the page's lookup knows it; the height only for a container of both
 * sizes.  Without a container, or its size, the viewport's (a container
 * the lookup does not know is counted as missed).
 */
static void
cascade_container(
	struct css_engine *engine,
	float *width,
	float *height)
{
	struct cascade_cached *cached;
	struct cascade_container_use use;
	struct cascade_container_use *last;
	struct dom_node *node;
	float found_width;
	float found_height;
	int known;
	int error;

	/* The viewport's size, unless a container says otherwise. */
	*width = engine->viewport_width;
	*height = engine->viewport_height;
	if (engine->current == NULL || engine->style_capacity == 0)
		return;

	/* The element's ancestors, nearest first. */
	for (node = engine->current->node.parent; node != NULL && node->type == DOM_ELEMENT; node = node->parent) {
		cached = cascade_style_slot(engine, (struct dom_element *)node);
		if (cached->element == NULL || cached->style->container_type == CSS_CONTAINER_NORMAL)
			continue;

		/* The container: its size as the page knows it. */
		known = 0;
		if (engine->container_lookup != NULL)
			known = engine->container_lookup(engine->container_context, (struct dom_element *)node, &found_width, &found_height);
		if (!known) {
			engine->container_missed++;
			return;
		}

		/* Its width, and its height when it contains both. */
		*width = found_width;
		if (cached->style->container_type == CSS_CONTAINER_SIZE)
			*height = found_height;

		/* The size used is remembered (once for a container used again in a row). */
		use.element = (struct dom_element *)node;
		use.width = found_width;
		use.height = found_height;
		last = NULL;
		if (engine->container_uses.count != 0)
			last = wb_vector_at(&engine->container_uses, engine->container_uses.count - 1U);
		if (last == NULL || last->element != use.element) {
			error = wb_vector_push(&engine->container_uses, &use);
			if (error != 0)
				engine->container_missed++;
		}

		/* The container is found. */
		return;
	}
}

/*
 * Computes a min(), max() or clamp() inside a sum in pixels:
 * a percentage it leaves is measured against the viewport's width, as the
 * comparisons of cascade_calc are.
 */
static float
cascade_calc_pixels(
	struct css_engine *engine,
	const struct css_calc *calc,
	float font_size)
{
	struct css_length length;

	/* The calculation, then its percentage in pixels. */
	length = cascade_calc(engine, calc, font_size);
	if (length.unit == CSS_UNIT_PERCENT)
		return length.value * engine->viewport_width / 100.0f + length.offset;

	/* The pixels. */
	return length.value;
}

/* Orders two applying declarations: rank, then specificity, then order. */
static int
cascade_compare(
	const void *left,
	const void *right)
{
	const struct cascade_match *a;
	const struct cascade_match *b;

	/* Compares field by field: a lower rank, specificity or order comes first. */
	a = left;
	b = right;
	if (a->rank < b->rank)
		return -1;
	if (a->rank > b->rank)
		return 1;
	if (a->specificity < b->specificity)
		return -1;
	if (a->specificity > b->specificity)
		return 1;
	if (a->order < b->order)
		return -1;
	if (a->order > b->order)
		return 1;

	/* The same declaration. */
	return 0;
}

/*
 * Tells whether a candidate selector of the index applies to what is being
 * styled: the element itself, or its ::before or ::after.  While the
 * element itself is styled, a selector of its ::before or ::after that
 * matches marks the element's pseudo_seen instead.
 */
static int
cascade_candidate_matches(
	struct css_engine *engine,
	struct dom_element *element,
	const struct css_selector *selector)
{
	int pseudo;
	int matches;

	/* A selector for what is being styled is matched as it is. */
	pseudo = selector->compounds[selector->count - 1U].pseudo_element;
	if (pseudo == engine->pseudo_wanted) {
		matches = cascade_selector_matches(engine, element, selector, selector->count - 1U, NULL);
		return matches;
	}

	/* Only the element's own styling looks at the others, to see which pseudo-elements it has. */
	if (engine->pseudo_wanted != CSS_PSEUDO_ELEMENT_NONE || pseudo == CSS_PSEUDO_ELEMENT_OTHER)
		return 0;

	/* A ::before or ::after selector that would match marks the element. */
	engine->pseudo_wanted = pseudo;
	matches = cascade_selector_matches(engine, element, selector, selector->count - 1U, NULL);
	engine->pseudo_wanted = CSS_PSEUDO_ELEMENT_NONE;
	if (matches)
		engine->pseudo_seen |= 1 << pseudo;

	/* Its declarations are not the element's. */
	return 0;
}

/*
 * Tells whether an element matches a selector's compounds up to index,
 * right to left.  With an anchor (the element a :has() is on), the first
 * compound must also stand in its combinator's relation to the anchor.
 */
static int
cascade_selector_matches(
	struct css_engine *engine,
	struct dom_element *element,
	const struct css_selector *selector,
	size_t index,
	struct dom_element *anchor)
{
	const struct css_compound *compound;
	struct dom_element *other;
	int matches;

	/* The compound at index must match the element itself. */
	compound = &selector->compounds[index];
	matches = cascade_compound_matches(engine, element, compound);
	if (!matches)
		return 0;

	/* The first compound ends the selector, or relates it to the anchor. */
	if (index == 0) {
		if (anchor == NULL)
			return 1;
		matches = cascade_anchored(element, compound->combinator, anchor);
		return matches;
	}

	/* Then the compound on its left, through the combinator. */
	switch (compound->combinator) {
	case CSS_COMBINATOR_CHILD:
		other = cascade_parent_element(element);
		if (other == NULL || other == anchor)
			return 0;
		return cascade_selector_matches(engine, other, selector, index - 1U, anchor);
	case CSS_COMBINATOR_DESCENDANT:
		for (other = cascade_parent_element(element); other != NULL && other != anchor; other = cascade_parent_element(other)) {
			matches = cascade_selector_matches(engine, other, selector, index - 1U, anchor);
			if (matches)
				return 1;
		}

		/* Otherwise no descendant relation holds. */
		return 0;
	case CSS_COMBINATOR_NEXT:
		other = cascade_previous_element(element);
		if (other == NULL || other == anchor)
			return 0;
		return cascade_selector_matches(engine, other, selector, index - 1U, anchor);
	case CSS_COMBINATOR_SUBSEQUENT:
		for (other = cascade_previous_element(element); other != NULL && other != anchor; other = cascade_previous_element(other)) {
			matches = cascade_selector_matches(engine, other, selector, index - 1U, anchor);
			if (matches)
				return 1;
		}

		/* Otherwise no earlier sibling matched. */
		return 0;
	default:
		return 0;
	}
}

/* Tells whether an element stands in a combinator's relation to an anchor (on the combinator's left). */
static int
cascade_anchored(
	struct dom_element *element,
	int combinator,
	struct dom_element *anchor)
{
	struct dom_element *other;

	/* The relation. */
	switch (combinator) {
	case CSS_COMBINATOR_CHILD:
		other = cascade_parent_element(element);
		return other == anchor;
	case CSS_COMBINATOR_NEXT:
		other = cascade_previous_element(element);
		return other == anchor;
	case CSS_COMBINATOR_SUBSEQUENT:
		for (other = cascade_previous_element(element); other != NULL; other = cascade_previous_element(other)) {
			if (other == anchor)
				return 1;
		}

		/* The anchor is not an earlier sibling. */
		return 0;
	default:
		for (other = cascade_parent_element(element); other != NULL; other = cascade_parent_element(other)) {
			if (other == anchor)
				return 1;
		}

		/* The anchor is not an ancestor. */
		return 0;
	}
}

/* Tells whether an element matches every simple selector of a compound. */
static int
cascade_compound_matches(
	struct css_engine *engine,
	struct dom_element *element,
	const struct css_compound *compound)
{
	size_t index;
	int matches;

	/* Every simple selector must match. */
	for (index = 0; index < compound->count; index++) {
		matches = cascade_simple_matches(engine, element, &compound->simples[index]);
		if (!matches)
			return 0;
	}

	/* The compound matches. */
	return 1;
}

/* Tells whether an element matches one simple selector. */
static int
cascade_simple_matches(
	struct css_engine *engine,
	struct dom_element *element,
	const struct css_simple *simple)
{
	struct dom_attribute *attribute;
	struct vm_string *const *atoms;
	size_t count;
	size_t position;
	int matches;
	int insensitive;
	int html_type;
	int error;

	/* The kind of selector. */
	switch (simple->kind) {
	case CSS_SIMPLE_UNIVERSAL:
		return 1;
	case CSS_SIMPLE_TYPE:
		/* HTML element names match case-insensitively; theirs are lower case already. */
		return element->local_name == simple->name;
	case CSS_SIMPLE_ID:
		attribute = dom_element_find_attribute(element, DOM_NS_NONE, engine->atom_id);
		if (attribute == NULL)
			return 0;
		return vm_string_equal(attribute->value, simple->name);
	case CSS_SIMPLE_CLASS:
		attribute = dom_element_find_attribute(element, DOM_NS_NONE, engine->atom_class);
		if (attribute == NULL)
			return 0;

		/* The attribute's words as atoms (a failure to keep them falls back to comparing characters). */
		error = cascade_class_atoms(engine, attribute->value, &atoms, &count);
		if (error != 0)
			return cascade_has_class(attribute->value, simple->name);

		/* The selector's name is an atom: one of the words' atoms is it, or none is. */
		for (position = 0; position < count; position++) {
			if (atoms[position] == simple->name)
				return 1;
		}

		/* The element does not have the class. */
		return 0;
	case CSS_SIMPLE_ATTRIBUTE:
		attribute = dom_element_find_attribute(element, DOM_NS_NONE, simple->name);
		if (attribute == NULL)
			return 0;

		/* HTML type values match without case unless an explicit s flag overrides the host language. */
		html_type = element->ns == DOM_NS_HTML && element->node.document->content == DOM_CONTENT_HTML &&
		    vm_string_equal_ascii(simple->name, "type");
		insensitive = simple->case_insensitive;
		if (!simple->case_sensitive && html_type)
			insensitive = 1;
		return cascade_attribute_matches(attribute->value, simple, insensitive, html_type);
	case CSS_SIMPLE_PSEUDO_CLASS:
		matches = cascade_pseudo_class(engine, element, simple);
		return matches;
	case CSS_SIMPLE_PSEUDO_ELEMENT:
		/* A pseudo-element matches while its own style is computed. */
		return simple->pseudo == engine->pseudo_wanted;
	default:
		return 0;
	}
}

/* Finds the nearest declared HTML or namespaced XML language for this element. */
static int
cascade_language(
	struct dom_element *element,
	const struct vm_string *range)
{
	struct dom_element *ancestor;
	struct dom_attribute *attribute;
	struct vm_string *language;
	size_t index;
	int same;
	int matches;

	/* Walks the actual element ancestry, stopping at the first declared language. */
	ancestor = element;
	while (ancestor != NULL) {
		/* A namespaced XML language takes precedence over an HTML lang attribute. */
		language = NULL;
		for (index = 0; index < ancestor->attribute_count; index++) {
			attribute = &ancestor->attributes[index];
			if (attribute->ns != DOM_NS_XML)
				continue;

			/* Only XML's lang local name defines inherited human language. */
			same = vm_string_equal_ascii(attribute->name, "lang");
			if (same) {
				language = attribute->value;
				break;
			}
		}

		/* HTML elements may declare language in their ordinary lang attribute. */
		if (language == NULL && ancestor->ns == DOM_NS_HTML)
			language = dom_attribute_ascii(ancestor, "lang");

		/* An explicit empty language stops inheritance and matches no nonempty range. */
		if (language != NULL) {
			matches = cascade_language_matches(language, range);
			if (!matches)
				return 0;

			/* The nearest declaration matches without consulting more distant ancestors. */
			return 1;
		}

		/* Inherits only when this element has no language declaration of its own. */
		ancestor = cascade_parent_element(ancestor);
	}

	/* No document metadata or protocol language is available in this embedding. */
	return 0;
}

/* Compares a language with a single range using ASCII folding and a hyphen boundary. */
static int
cascade_language_matches(
	const struct vm_string *language,
	const struct vm_string *range)
{
	size_t index;
	uint16_t actual;
	uint16_t expected;

	/* A nonempty range cannot match a shorter or explicitly unknown language. */
	if (range == NULL || range->length == 0)
		return 0;

	/* The complete range must fit before inspecting a following boundary. */
	if (language->length < range->length)
		return 0;

	/* Language-range comparison folds ASCII only, without allocating VM strings. */
	for (index = 0; index < range->length; index++) {
		actual = vm_string_at(language, index);
		expected = vm_string_at(range, index);

		/* Uppercase ASCII in the declared language has no semantic difference. */
		if (actual >= 'A' && actual <= 'Z')
			actual += 'a' - 'A';

		/* Applies the same ASCII folding to the selector's range. */
		if (expected >= 'A' && expected <= 'Z')
			expected += 'a' - 'A';

		/* A differing unit prevents both exact and hyphen-prefix matches. */
		if (actual != expected)
			return 0;
	}

	/* Exact language equality needs no suffix separator. */
	if (language->length == range->length)
		return 1;

	/* A longer language matches only at a language-subtag separator. */
	actual = vm_string_at(language, range->length);
	if (actual != '-')
		return 0;

	/* Succeeded: the selector names this language or one of its parent ranges. */
	return 1;
}

/* Tells whether an element matches a pseudo-class. */
static int
cascade_pseudo_class(
	struct css_engine *engine,
	struct dom_element *element,
	const struct css_simple *simple)
{
	struct dom_node *child;
	const struct dom_character_data *text;
	struct vm_string *attribute;
	struct wb_units value;
	int kind;
	int matches;
	int error;

	/* The pseudo-class. */
	switch (simple->pseudo) {
	case CSS_PSEUDO_ROOT:
		return element->node.parent != NULL && element->node.parent->type == DOM_DOCUMENT;
	case CSS_PSEUDO_FIRST_CHILD:
		return cascade_previous_element(element) == NULL && cascade_parent_element(element) != NULL;
	case CSS_PSEUDO_LAST_CHILD:
		return cascade_next_element(element) == NULL && cascade_parent_element(element) != NULL;
	case CSS_PSEUDO_ONLY_CHILD:
		return cascade_previous_element(element) == NULL && cascade_next_element(element) == NULL;
	case CSS_PSEUDO_LANG:
		/* The nearest declared language controls both this element and its descendants. */
		matches = cascade_language(element, simple->value);
		return matches;
	case CSS_PSEUDO_EMPTY:
		/* Elements and text containing any data prevent structural emptiness. */
		for (child = element->node.first_child; child != NULL; child = child->next) {
			/* Any namespace's element is real structural content. */
			if (child->type == DOM_ELEMENT)
				return 0;

			/* Empty text nodes and comments do not contribute content. */
			if (child->type == DOM_TEXT) {
				text = (const struct dom_character_data *)child;
				if (text->data.length != 0)
					return 0;
			}
		}

		/* No element or nonempty text child prevents the match. */
		return 1;
	case CSS_PSEUDO_LINK:
	case CSS_PSEUDO_VISITED:
	case CSS_PSEUDO_ANY_LINK:
		/* Link history applies only to HTML hyperlinks with an href. */
		if (element->ns != DOM_NS_HTML || (element->tag != DOM_TAG_A && element->tag != DOM_TAG_AREA))
			return 0;
		if (dom_element_find_attribute(element, DOM_NS_NONE, engine->atom_href) == NULL)
			return 0;

		/* Script-visible matching is intentionally always unvisited. */
		if (simple->pseudo == CSS_PSEUDO_ANY_LINK)
			return 1;
		if (!engine->visited_pass)
			return simple->pseudo == CSS_PSEUDO_LINK;

		/* The visited pass uses only navigations the owning page observed. */
		matches = engine->visited_lookup(engine->visited_context, element);
		if (simple->pseudo == CSS_PSEUDO_VISITED)
			return matches;
		return !matches;
	case CSS_PSEUDO_NOT:
		matches = cascade_any_matches(engine, element, simple);
		return !matches;
	case CSS_PSEUDO_IS:
		matches = cascade_any_matches(engine, element, simple);
		return matches;
	case CSS_PSEUDO_HAS:
		matches = cascade_has(engine, element, simple);
		return matches;
	case CSS_PSEUDO_NTH_CHILD:
	case CSS_PSEUDO_NTH_LAST_CHILD:
	case CSS_PSEUDO_NTH_OF_TYPE:
	case CSS_PSEUDO_NTH_LAST_OF_TYPE:
	case CSS_PSEUDO_FIRST_OF_TYPE:
	case CSS_PSEUDO_LAST_OF_TYPE:
		matches = cascade_nth(element, simple);
		return matches;
	case CSS_PSEUDO_ONLY_OF_TYPE:
		matches = cascade_nth(element, simple);
		return matches;
	case CSS_PSEUDO_DISABLED:
	case CSS_PSEUDO_ENABLED:
		/* A form element is disabled by its attribute. */
		matches = cascade_is_form_element(element);
		if (!matches)
			return 0;
		attribute = dom_attribute_ascii(element, "disabled");
		if (simple->pseudo == CSS_PSEUDO_DISABLED)
			return attribute != NULL;
		return attribute == NULL;
	case CSS_PSEUDO_CHECKED:
		/* A checked checkbox or radio button, or a selected option. */
		if (element->ns == DOM_NS_HTML && element->tag == DOM_TAG_OPTION) {
			attribute = dom_attribute_ascii(element, "selected");
			return attribute != NULL;
		}

		/* Otherwise only a checkbox or a radio button can be checked. */
		kind = dom_control_kind(element);
		if (kind != DOM_CONTROL_CHECKBOX && kind != DOM_CONTROL_RADIO)
			return 0;
		matches = dom_control_checked(element);
		return matches;
	case CSS_PSEUDO_PLACEHOLDER_SHOWN:
		/* A field with a placeholder and no value. */
		attribute = dom_attribute_ascii(element, "placeholder");
		if (attribute == NULL)
			return 0;
		wb_units_init(&value);
		error = dom_control_value(element, &value);
		matches = 0;
		if (error == 0 && value.length == 0)
			matches = 1;
		wb_units_release(&value);
		return matches;
	case CSS_PSEUDO_REQUIRED:
	case CSS_PSEUDO_OPTIONAL:
		/* A form element is required by its attribute. */
		matches = cascade_is_form_element(element);
		if (!matches)
			return 0;
		attribute = dom_attribute_ascii(element, "required");
		if (simple->pseudo == CSS_PSEUDO_REQUIRED)
			return attribute != NULL;
		return attribute == NULL;
	case CSS_PSEUDO_ALWAYS:
		return 1;
	default:
		return 0;
	}
}

/* Tells whether an element matches any selector of an :is() or :not() argument list. */
static int
cascade_any_matches(
	struct css_engine *engine,
	struct dom_element *element,
	const struct css_simple *simple)
{
	const struct css_selector *selector;
	size_t index;
	int matches;

	/* Each selector of the list, as for the element itself. */
	for (index = 0; index < simple->argument_count; index++) {
		selector = &simple->arguments[index];
		if (selector->count == 0)
			continue;
		matches = cascade_selector_matches(engine, element, selector, selector->count - 1U, NULL);
		if (matches)
			return 1;
	}

	/* None matched. */
	return 0;
}

/*
 * Tells whether an element has what a :has() argument asks for: an
 * element among its descendants, or its later siblings and theirs, that a
 * relative selector matches anchored at the element.
 */
static int
cascade_has(
	struct css_engine *engine,
	struct dom_element *element,
	const struct css_simple *simple)
{
	struct dom_node *sibling;
	size_t index;
	int siblings;
	int matches;

	/* The descendants. */
	matches = cascade_has_below(engine, &element->node, simple, element, 0);
	if (matches)
		return 1;

	/* The later siblings and their descendants, when a selector starts with + or ~. */
	siblings = 0;
	for (index = 0; index < simple->argument_count; index++) {
		if (simple->arguments[index].compounds[0].combinator == CSS_COMBINATOR_NEXT)
			siblings = 1;
		if (simple->arguments[index].compounds[0].combinator == CSS_COMBINATOR_SUBSEQUENT)
			siblings = 1;
	}

	/* Without one, the siblings are not looked at. */
	if (!siblings)
		return 0;

	/* Each later sibling, itself and below. */
	for (sibling = element->node.next; sibling != NULL; sibling = sibling->next) {
		if (sibling->type != DOM_ELEMENT)
			continue;
		matches = cascade_any_relative(engine, (struct dom_element *)sibling, simple, element);
		if (matches)
			return 1;
		matches = cascade_has_below(engine, sibling, simple, element, 0);
		if (matches)
			return 1;
	}

	/* Nothing matched. */
	return 0;
}

/* Tells whether an element below a node (at any depth) matches a relative selector of :has() anchored at anchor. */
static int
cascade_has_below(
	struct css_engine *engine,
	struct dom_node *node,
	const struct css_simple *simple,
	struct dom_element *anchor,
	int depth)
{
	struct dom_node *child;
	int matches;

	/* Stops at the depth the parser stops at. */
	if (depth > CASCADE_HAS_DEPTH)
		return 0;

	/* Each element child, itself then its own descendants. */
	for (child = node->first_child; child != NULL; child = child->next) {
		if (child->type != DOM_ELEMENT)
			continue;
		matches = cascade_any_relative(engine, (struct dom_element *)child, simple, anchor);
		if (matches)
			return 1;
		matches = cascade_has_below(engine, child, simple, anchor, depth + 1);
		if (matches)
			return 1;
	}

	/* Nothing below matched. */
	return 0;
}

/* Tells whether an element matches any relative selector of a :has() anchored at anchor. */
static int
cascade_any_relative(
	struct css_engine *engine,
	struct dom_element *element,
	const struct css_simple *simple,
	struct dom_element *anchor)
{
	const struct css_selector *selector;
	size_t index;
	int matches;

	/* Each relative selector. */
	for (index = 0; index < simple->argument_count; index++) {
		selector = &simple->arguments[index];
		matches = cascade_selector_matches(engine, element, selector, selector->count - 1U, anchor);
		if (matches)
			return 1;
	}

	/* None matched. */
	return 0;
}

/*
 * Tells whether an element's place among its siblings matches an
 * :nth-*() (an+b, counting from 1, of every element or of its type, from
 * the start or the end) or a :*-of-type pseudo-class.
 */
static int
cascade_nth(
	struct dom_element *element,
	const struct css_simple *simple)
{
	struct dom_node *node;
	struct dom_element *other;
	int of_type;
	int from_end;
	int place;
	int after;
	int step;

	/* What is counted, and from which end. */
	of_type = 0;
	from_end = 0;
	if (simple->pseudo == CSS_PSEUDO_NTH_OF_TYPE ||
	    simple->pseudo == CSS_PSEUDO_NTH_LAST_OF_TYPE ||
	    simple->pseudo == CSS_PSEUDO_FIRST_OF_TYPE ||
	    simple->pseudo == CSS_PSEUDO_LAST_OF_TYPE ||
	    simple->pseudo == CSS_PSEUDO_ONLY_OF_TYPE)
		of_type = 1;
	if (simple->pseudo == CSS_PSEUDO_NTH_LAST_CHILD ||
	    simple->pseudo == CSS_PSEUDO_NTH_LAST_OF_TYPE ||
	    simple->pseudo == CSS_PSEUDO_LAST_OF_TYPE)
		from_end = 1;

	/* A root, or an element without an element parent, is not counted among siblings. */
	other = cascade_parent_element(element);
	if (other == NULL)
		return 0;

	/* The places before and after the element (1 for the first). */
	place = 1;
	for (node = element->node.previous; node != NULL; node = node->previous) {
		if (node->type != DOM_ELEMENT)
			continue;
		other = (struct dom_element *)node;
		if (!of_type || other->local_name == element->local_name)
			place++;
	}

	/* The places after it, counted the same way. */
	after = 1;
	for (node = element->node.next; node != NULL; node = node->next) {
		if (node->type != DOM_ELEMENT)
			continue;
		other = (struct dom_element *)node;
		if (!of_type || other->local_name == element->local_name)
			after++;
	}

	/* The :*-of-type forms. */
	if (simple->pseudo == CSS_PSEUDO_FIRST_OF_TYPE)
		return place == 1;
	if (simple->pseudo == CSS_PSEUDO_LAST_OF_TYPE)
		return after == 1;
	if (simple->pseudo == CSS_PSEUDO_ONLY_OF_TYPE)
		return place == 1 && after == 1;

	/* an+b from the chosen end: some n >= 0 gives the place. */
	if (from_end)
		place = after;
	if (simple->nth_a == 0)
		return place == simple->nth_b;
	step = place - simple->nth_b;
	if (step % simple->nth_a != 0)
		return 0;

	/* n = step / a must not be negative. */
	return step / simple->nth_a >= 0;
}

/* Tells whether an element is an HTML form element that can be disabled or required. */
static int
cascade_is_form_element(
	const struct dom_element *element)
{
	/* The HTML form elements. */
	if (element->ns != DOM_NS_HTML)
		return 0;
	switch (element->tag) {
	case DOM_TAG_INPUT:
	case DOM_TAG_BUTTON:
	case DOM_TAG_SELECT:
	case DOM_TAG_TEXTAREA:
	case DOM_TAG_OPTION:
	case DOM_TAG_FIELDSET:
		return 1;
	default:
		return 0;
	}
}

/* Compares an attribute's value with an attribute selector's. */
static int
cascade_attribute_matches(
	const struct vm_string *value,
	const struct css_simple *simple,
	int insensitive,
	int html_type)
{
	const struct vm_string *wanted;
	size_t start;
	size_t index;
	size_t length;
	uint16_t a;
	uint16_t b;

	/* Existence alone. */
	if (simple->match == CSS_MATCH_EXISTS)
		return 1;
	wanted = simple->value;

	/* Keep existing list matching for other attributes outside this HTML type change. */
	if (simple->match == CSS_MATCH_INCLUDES && !html_type)
		return cascade_has_class(value, wanted);

	/* Compares at the place the kind of match asks for. */
	length = wanted->length;
	if (simple->match == CSS_MATCH_INCLUDES && length == 0)
		return 0;
	if (simple->match == CSS_MATCH_EQUAL && value->length != length)
		return 0;
	if (value->length < length)
		return 0;
	if (simple->match == CSS_MATCH_DASH && value->length > length) {
		a = vm_string_at(value, length);
		if (a != '-')
			return 0;
	}

	/* Tries each place the value may start at. */
	for (start = 0; start + length <= value->length; start++) {
		if (simple->match == CSS_MATCH_SUFFIX && start != value->length - length)
			continue;
		if (simple->match == CSS_MATCH_INCLUDES) {
			/* A list match spans a complete CSS-whitespace-separated word. */
			if (start > 0) {
				a = vm_string_at(value, start - 1U);
				if (a != ' ' && a != '\t' && a != '\n' && a != '\f' && a != '\r')
					continue;
			}
			if (start + length < value->length) {
				a = vm_string_at(value, start + length);
				if (a != ' ' && a != '\t' && a != '\n' && a != '\f' && a != '\r')
					continue;
			}
		}
		for (index = 0; index < length; index++) {
			a = vm_string_at(value, start + index);
			b = vm_string_at(wanted, index);
			if (insensitive) {
				if (a >= 'A' && a <= 'Z')
					a = (uint16_t)(a + 0x20U);
				if (b >= 'A' && b <= 'Z')
					b = (uint16_t)(b + 0x20U);
			}

			/* The first difference ends the comparison at this place. */
			if (a != b)
				break;
		}

		/* The whole value matched here. */
		if (index == length)
			return 1;
		if (simple->match != CSS_MATCH_SUBSTRING && simple->match != CSS_MATCH_SUFFIX && simple->match != CSS_MATCH_INCLUDES)
			return 0;
	}

	/* No place matched. */
	return 0;
}

/*
 * Finds a class attribute's words as the atoms some sheet made for them
 * (in the attribute's order; words no sheet names are left out), splitting
 * the attribute the first time the engine meets its string and keeping
 * the atoms for as long as the engine lives.
 */
static int
cascade_class_atoms(
	struct css_engine *engine,
	struct vm_string *classes,
	struct vm_string *const **atoms,
	size_t *count)
{
	struct cascade_classes *slot;
	struct vm_string *atom;
	size_t start;
	size_t end;
	uint16_t unit;
	uint32_t first;
	int error;

	/* Grows the table when adding would fill more than half of it. */
	if ((engine->class_count + 1U) * 2U > engine->class_capacity) {
		error = cascade_class_grow(engine);
		if (error != 0)
			return error;
	}

	/* A string met before has its atoms. */
	slot = cascade_class_slot(engine, classes);
	if (slot->classes != NULL) {
		*atoms = (struct vm_string *const *)engine->class_atoms.items + slot->start;
		*count = slot->count;
		return 0;
	}

	/* Otherwise each whitespace-separated word's atom, when a sheet made one, joins the run. */
	first = (uint32_t)engine->class_atoms.count;
	start = 0;
	while (start < classes->length) {
		/* Skips whitespace before the word. */
		unit = vm_string_at(classes, start);
		if (unit == ' ' || unit == '\t' || unit == '\n' || unit == '\f' || unit == '\r') {
			start++;
			continue;
		}

		/* Copies the word's characters up to the next whitespace. */
		wb_units_clear(&engine->word);
		end = start;
		while (end < classes->length) {
			unit = vm_string_at(classes, end);
			if (unit == ' ' || unit == '\t' || unit == '\n' || unit == '\f' || unit == '\r')
				break;
			error = wb_units_append(&engine->word, &unit, 1);
			if (error != 0)
				return ENOMEM;
			end++;
		}

		/* The word's atom, when some sheet named it. */
		atom = vm_atom_find_units(engine->heap, engine->word.data, engine->word.length);
		if (atom != NULL) {
			error = wb_vector_push(&engine->class_atoms, &atom);
			if (error != 0)
				return ENOMEM;
		}

		/* On to the next word. */
		start = end;
	}

	/* The string's entry: where its run is. */
	slot->classes = classes;
	slot->length = classes->length;
	slot->hash = vm_string_hash(classes);
	slot->start = first;
	slot->count = (uint32_t)engine->class_atoms.count - first;
	engine->class_count++;

	/* Succeeded: reports the run. */
	*atoms = (struct vm_string *const *)engine->class_atoms.items + first;
	*count = slot->count;
	return 0;
}

/* Finds the table slot of a class attribute's string: the one holding it, or the empty one where it goes. */
static struct cascade_classes *
cascade_class_slot(
	struct css_engine *engine,
	struct vm_string *classes)
{
	struct cascade_classes *slot;
	uintptr_t key;
	uint32_t hash;
	size_t index;

	/* Probes linearly from the string's address. */
	key = (uintptr_t)classes;
	index = (size_t)((key >> 4) * 0x9e3779b97f4a7c15ULL >> 20) & (engine->class_capacity - 1U);
	for (;;) {
		slot = &engine->class_table[index];
		if (slot->classes == NULL)
			return slot;

		/* The same string, unchanged since it was split (its hash tells a new string at the same address). */
		if (slot->classes == classes && slot->length == classes->length) {
			hash = vm_string_hash(classes);
			if (slot->hash == hash)
				return slot;
		}

		/* The next slot of the probe. */
		index = (index + 1U) & (engine->class_capacity - 1U);
	}
}

/*
 * Doubles the class table and moves every entry (an entry whose string was
 * replaced at the same address is dropped with the others' moves, since it
 * is found again only by an equal string).
 */
static int
cascade_class_grow(
	struct css_engine *engine)
{
	struct cascade_classes *old;
	struct cascade_classes *slot;
	size_t old_capacity;
	size_t index;

	/* Takes the larger, empty table. */
	old = engine->class_table;
	old_capacity = engine->class_capacity;
	engine->class_capacity = old_capacity * 2U;
	if (engine->class_capacity < 256U)
		engine->class_capacity = 256U;
	engine->class_table = calloc(engine->class_capacity, sizeof(*engine->class_table));
	if (engine->class_table == NULL) {
		engine->class_table = old;
		engine->class_capacity = old_capacity;
		return ENOMEM;
	}

	/* Moves the entries into the slots their strings probe to. */
	for (index = 0; index < old_capacity; index++) {
		if (old[index].classes == NULL)
			continue;
		slot = cascade_class_slot(engine, old[index].classes);
		*slot = old[index];
	}

	/* The old table is no longer needed. */
	free(old);

	/* Succeeded: the table has room. */
	return 0;
}

/* Finds the style table's slot of an element: the one holding it, or the empty one where it goes. */
static struct cascade_cached *
cascade_style_slot(
	struct css_engine *engine,
	const struct dom_element *element)
{
	struct cascade_cached *slot;
	uintptr_t key;
	size_t index;

	/* Probes linearly from the element's address. */
	key = (uintptr_t)element;
	index = (size_t)((key >> 4) * 0x9e3779b97f4a7c15ULL >> 20) & (engine->style_capacity - 1U);
	for (;;) {
		slot = &engine->style_table[index];
		if (slot->element == NULL || slot->element == element)
			return slot;

		/* The next slot of the probe. */
		index = (index + 1U) & (engine->style_capacity - 1U);
	}
}

/* Keeps a copy of an element's computed style in the engine; running out of memory only leaves it out. */
static void
cascade_style_keep(
	struct css_engine *engine,
	struct dom_element *element,
	const struct css_style *style)
{
	struct cascade_cached *slot;
	struct css_style *copy;
	int error;

	/* Grows the table when adding would fill more than half of it. */
	if ((engine->style_count + 1U) * 2U > engine->style_capacity) {
		error = cascade_style_grow(engine);
		if (error != 0)
			return;
	}

	/* The copy. */
	copy = malloc(sizeof(*copy));
	if (copy == NULL)
		return;
	*copy = *style;

	/* Its slot (an element kept already keeps its first copy). */
	slot = cascade_style_slot(engine, element);
	if (slot->element != NULL) {
		free(copy);
		return;
	}

	/* The element's entry. */
	slot->element = element;
	slot->style = copy;
	engine->style_count++;
}

/* Doubles the style table and moves every entry. */
static int
cascade_style_grow(
	struct css_engine *engine)
{
	struct cascade_cached *old;
	struct cascade_cached *slot;
	size_t old_capacity;
	size_t index;

	/* Takes the larger, empty table. */
	old = engine->style_table;
	old_capacity = engine->style_capacity;
	engine->style_capacity = old_capacity * 2U;
	if (engine->style_capacity < 256U)
		engine->style_capacity = 256U;
	engine->style_table = calloc(engine->style_capacity, sizeof(*engine->style_table));
	if (engine->style_table == NULL) {
		engine->style_table = old;
		engine->style_capacity = old_capacity;
		return ENOMEM;
	}

	/* Moves the entries into the slots their elements probe to. */
	for (index = 0; index < old_capacity; index++) {
		if (old[index].element == NULL)
			continue;
		slot = cascade_style_slot(engine, old[index].element);
		*slot = old[index];
	}

	/* The old table is no longer needed. */
	free(old);

	/* Succeeded: the table has room. */
	return 0;
}

/* Forgets every style the engine kept (the table stays, empty). */
static void
cascade_style_forget(
	struct css_engine *engine)
{
	size_t index;

	/* Frees each copy and empties its slot. */
	for (index = 0; index < engine->style_capacity; index++) {
		free(engine->style_table[index].style);
		engine->style_table[index].element = NULL;
		engine->style_table[index].style = NULL;
	}

	/* No style is kept. */
	engine->style_count = 0;
}

/* Tells whether a whitespace-separated list holds a word. */
static int
cascade_has_class(
	const struct vm_string *classes,
	const struct vm_string *name)
{
	size_t start;
	size_t end;
	size_t index;
	uint16_t unit;
	uint16_t wanted;

	/* Walks the words. */
	start = 0;
	while (start < classes->length) {
		/* Skips whitespace, then finds the word's end. */
		unit = vm_string_at(classes, start);
		if (unit == ' ' || unit == '\t' || unit == '\n' || unit == '\f' || unit == '\r') {
			start++;
			continue;
		}

		/* Finds the end of the word. */
		end = start;
		while (end < classes->length) {
			unit = vm_string_at(classes, end);
			if (unit == ' ' || unit == '\t' || unit == '\n' || unit == '\f' || unit == '\r')
				break;
			end++;
		}

		/* A word of the same length and units is the class. */
		if (end - start == name->length) {
			for (index = 0; index < name->length; index++) {
				unit = vm_string_at(classes, start + index);
				wanted = vm_string_at(name, index);
				if (unit != wanted)
					break;
			}

			/* The whole word matched. */
			if (index == name->length)
				return 1;
		}

		/* On to the next word. */
		start = end;
	}

	/* The word is not in the list. */
	return 0;
}

/* Finds an element's parent element, or NULL. */
static struct dom_element *
cascade_parent_element(
	struct dom_element *element)
{
	/* Only an element parent counts. */
	if (element->node.parent == NULL || element->node.parent->type != DOM_ELEMENT)
		return NULL;

	/* Reports the parent. */
	return (struct dom_element *)element->node.parent;
}

/* Finds the element sibling before an element, or NULL. */
static struct dom_element *
cascade_previous_element(
	struct dom_element *element)
{
	struct dom_node *node;

	/* Walks back over the text and comments. */
	for (node = element->node.previous; node != NULL; node = node->previous) {
		if (node->type == DOM_ELEMENT)
			return (struct dom_element *)node;
	}

	/* No element before it. */
	return NULL;
}

/* Finds the element sibling after an element, or NULL. */
static struct dom_element *
cascade_next_element(
	struct dom_element *element)
{
	struct dom_node *node;

	/* Walks forward over the text and comments. */
	for (node = element->node.next; node != NULL; node = node->next) {
		if (node->type == DOM_ELEMENT)
			return (struct dom_element *)node;
	}

	/* No element after it. */
	return NULL;
}

/* Resolves the four logical margin names after the element's flow is computed. */
static int
cascade_margin_property(
	int property,
	int writing_mode,
	int direction)
{
	/* overflow-inline and overflow-block name the horizontal and vertical axes by the writing mode. */
	if (property == CSS_PROP_OVERFLOW_INLINE)
		return writing_mode == CSS_WRITING_HORIZONTAL_TB ? CSS_PROP_OVERFLOW_X : CSS_PROP_OVERFLOW_Y;
	if (property == CSS_PROP_OVERFLOW_BLOCK)
		return writing_mode == CSS_WRITING_HORIZONTAL_TB ? CSS_PROP_OVERFLOW_Y : CSS_PROP_OVERFLOW_X;
	if (writing_mode == CSS_WRITING_VERTICAL_LR) {
		switch (property) {
		case CSS_PROP_MARGIN_INLINE_START:
			return direction == CSS_DIRECTION_RTL ? CSS_PROP_MARGIN_BOTTOM : CSS_PROP_MARGIN_TOP;
		case CSS_PROP_MARGIN_INLINE_END:
			return direction == CSS_DIRECTION_RTL ? CSS_PROP_MARGIN_TOP : CSS_PROP_MARGIN_BOTTOM;
		case CSS_PROP_MARGIN_BLOCK_START:
			return CSS_PROP_MARGIN_LEFT;
		case CSS_PROP_MARGIN_BLOCK_END:
			return CSS_PROP_MARGIN_RIGHT;
		default:
			return property;
		}
	}

	/* Horizontal block flow has top/bottom block edges and direction-dependent inline edges. */
	switch (property) {
	case CSS_PROP_MARGIN_INLINE_START:
		return direction == CSS_DIRECTION_RTL ? CSS_PROP_MARGIN_RIGHT : CSS_PROP_MARGIN_LEFT;
	case CSS_PROP_MARGIN_INLINE_END:
		return direction == CSS_DIRECTION_RTL ? CSS_PROP_MARGIN_LEFT : CSS_PROP_MARGIN_RIGHT;
	case CSS_PROP_MARGIN_BLOCK_START:
		return CSS_PROP_MARGIN_TOP;
	case CSS_PROP_MARGIN_BLOCK_END:
		return CSS_PROP_MARGIN_BOTTOM;
	default:
		return property;
	}
}

/* Applies one declaration to the style being computed. */
static void
cascade_apply(
	struct css_engine *engine,
	struct css_style *style,
	const struct css_style *parent,
	const struct css_declaration *declaration)
{
	const struct css_value *value;
	struct css_style initial;
	struct css_grid_place *place;
	int property;
	int inherited;
	int side;
	int line;
	int span;
	struct vm_string *name;
	int parent_keyword;
	float parent_size;

	/* The CSS-wide keywords. */
	value = &declaration->value;
	property = cascade_margin_property(declaration->property, style->writing_mode, style->direction);
	if (value->kind == CSS_VALUE_INHERIT || value->kind == CSS_VALUE_UNSET) {
		/* Unset follows inheritance only for properties whose initial cascade inherits. */
		inherited = 0;
		if (property == CSS_PROP_COLOR ||
		    property == CSS_PROP_FONT_SIZE ||
		    property == CSS_PROP_FONT_WEIGHT ||
		    property == CSS_PROP_FONT_STYLE ||
		    property == CSS_PROP_FONT_FAMILY ||
		    property == CSS_PROP_LINE_HEIGHT ||
		    property == CSS_PROP_TEXT_ALIGN ||
		    property == CSS_PROP_TEXT_INDENT ||
		    property == CSS_PROP_WHITE_SPACE ||
		    property == CSS_PROP_TEXT_TRANSFORM ||
		    property == CSS_PROP_TEXT_SHADOW ||
		    property == CSS_PROP_TEXT_DECORATION_SKIP_INK ||
		    property == CSS_PROP_CURSOR ||
		    property == CSS_PROP_VISIBILITY ||
		    property == CSS_PROP_DIRECTION ||
		    property == CSS_PROP_WRITING_MODE ||
		    property == CSS_PROP_TEXT_ORIENTATION ||
		    property == CSS_PROP_BORDER_SPACING_X ||
		    property == CSS_PROP_BORDER_SPACING_Y ||
		    property == CSS_PROP_BORDER_COLLAPSE ||
		    property == CSS_PROP_LIST_STYLE_TYPE)
			inherited = 1;

		/* Explicit inheritance and inherited unset take the parent value. */
		if (value->kind == CSS_VALUE_INHERIT || inherited) {
			if (parent != NULL)
				cascade_inherit(style, parent, property);
			return;
		}
	}

	/* The initial value. */
	if (value->kind == CSS_VALUE_INITIAL || value->kind == CSS_VALUE_UNSET) {
		css_initial_style(&initial);
		cascade_inherit(style, &initial, property);
		return;
	}

	/* The property's value. */
	switch (property) {
	case CSS_PROP_DISPLAY:
		style->display = value->keyword;
		break;
	case CSS_PROP_POSITION:
		style->position = value->keyword;
		break;
	case CSS_PROP_FLOAT:
		style->float_side = value->keyword;
		break;
	case CSS_PROP_CLEAR:
		style->clear = value->keyword;
		break;
	case CSS_PROP_OVERFLOW:
		style->overflow_x = value->keyword;
		style->overflow_y = value->keyword;
		break;
	case CSS_PROP_OVERFLOW_X:
		style->overflow_x = value->keyword;
		break;
	case CSS_PROP_OVERFLOW_Y:
		style->overflow_y = value->keyword;
		break;
	case CSS_PROP_VISIBILITY:
		style->visibility = value->keyword;
		break;
	case CSS_PROP_WIDTH:
		style->width = cascade_length(engine, value, style->font_size);
		break;
	case CSS_PROP_HEIGHT:
		style->height = cascade_length(engine, value, style->font_size);
		break;
	case CSS_PROP_MIN_WIDTH:
		style->min_width = cascade_length(engine, value, style->font_size);
		break;
	case CSS_PROP_MAX_WIDTH:
		style->max_width = cascade_length(engine, value, style->font_size);
		break;
	case CSS_PROP_MIN_HEIGHT:
		style->min_height = cascade_length(engine, value, style->font_size);
		break;
	case CSS_PROP_MAX_HEIGHT:
		style->max_height = cascade_length(engine, value, style->font_size);
		break;
	case CSS_PROP_MARGIN_TOP:
	case CSS_PROP_MARGIN_RIGHT:
	case CSS_PROP_MARGIN_BOTTOM:
	case CSS_PROP_MARGIN_LEFT:
		style->margin[property - CSS_PROP_MARGIN_TOP] = cascade_length(engine, value, style->font_size);
		break;
	case CSS_PROP_PADDING_TOP:
	case CSS_PROP_PADDING_RIGHT:
	case CSS_PROP_PADDING_BOTTOM:
	case CSS_PROP_PADDING_LEFT:
		style->padding[property - CSS_PROP_PADDING_TOP] = cascade_length(engine, value, style->font_size);
		break;
	case CSS_PROP_TOP:
	case CSS_PROP_RIGHT:
	case CSS_PROP_BOTTOM:
	case CSS_PROP_LEFT:
		style->offset[property - CSS_PROP_TOP] = cascade_length(engine, value, style->font_size);
		break;
	case CSS_PROP_Z_INDEX:
		style->z_index_auto = 1;
		style->z_index = 0;
		if (value->kind == CSS_VALUE_NUMBER)
			style->z_index_auto = 0;
		if (value->kind == CSS_VALUE_NUMBER)
			style->z_index = (int)value->number;
		break;
	case CSS_PROP_BORDER_TOP_WIDTH:
	case CSS_PROP_BORDER_RIGHT_WIDTH:
	case CSS_PROP_BORDER_BOTTOM_WIDTH:
	case CSS_PROP_BORDER_LEFT_WIDTH:
		style->border_width[property - CSS_PROP_BORDER_TOP_WIDTH] = cascade_length(engine, value, style->font_size).value;
		break;
	case CSS_PROP_BORDER_TOP_STYLE:
	case CSS_PROP_BORDER_RIGHT_STYLE:
	case CSS_PROP_BORDER_BOTTOM_STYLE:
	case CSS_PROP_BORDER_LEFT_STYLE:
		style->border_style[property - CSS_PROP_BORDER_TOP_STYLE] = value->keyword;
		break;
	case CSS_PROP_BORDER_TOP_COLOR:
	case CSS_PROP_BORDER_RIGHT_COLOR:
	case CSS_PROP_BORDER_BOTTOM_COLOR:
	case CSS_PROP_BORDER_LEFT_COLOR:
		style->border_color[property - CSS_PROP_BORDER_TOP_COLOR] = value->color;
		break;
	case CSS_PROP_COLOR:
		style->color = value->color;
		if (value->color == CSS_CURRENT_COLOR) {
			/* currentcolor in color itself is the parent's color. */
			style->color = 0xff000000U;
			if (parent != NULL)
				style->color = parent->color;
		}

		break;
	case CSS_PROP_BACKGROUND_COLOR:
		style->background_color = value->color;
		break;
	case CSS_PROP_BACKGROUND_IMAGE:
		style->background_image = NULL;
		if (value->kind == CSS_VALUE_URL)
			style->background_image = value->url;
		break;
	case CSS_PROP_BACKGROUND_REPEAT:
		style->background_repeat = value->keyword;
		break;
	case CSS_PROP_BACKGROUND_ATTACHMENT:
		style->background_attachment = value->keyword;
		break;
	case CSS_PROP_BACKGROUND_POSITION_X:
	case CSS_PROP_BACKGROUND_POSITION_Y:
		style->background_position[property - CSS_PROP_BACKGROUND_POSITION_X] = cascade_length(engine, value, style->font_size);
		break;
	case CSS_PROP_BACKGROUND_SIZE_WIDTH:
		/* contain and cover size both sides; otherwise the width. */
		style->background_size_keyword = CSS_BACKGROUND_SIZE_LENGTHS;
		if (value->kind == CSS_VALUE_KEYWORD && (value->keyword == CSS_BACKGROUND_SIZE_CONTAIN || value->keyword == CSS_BACKGROUND_SIZE_COVER)) {
			style->background_size_keyword = value->keyword;
			style->background_size[0].unit = CSS_UNIT_AUTO;
			break;
		}

		/* A width of lengths. */
		style->background_size[0] = cascade_length(engine, value, style->font_size);
		break;
	case CSS_PROP_BACKGROUND_SIZE_HEIGHT:
		style->background_size[1] = cascade_length(engine, value, style->font_size);
		break;
	case CSS_PROP_FONT_SIZE:
		/* The root measures against the default size, on the keyword scale. */
		parent_size = CASCADE_DEFAULT_FONT_SIZE;
		parent_keyword = 1;
		if (parent != NULL) {
			parent_size = parent->font_size;
			parent_keyword = parent->font_size_keyword;
		}

		/* The size, and whether it still follows the keyword scale. */
		style->font_size = cascade_font_size(engine, value, parent_size);
		style->font_size_keyword = cascade_font_size_keyword(value, parent_keyword);
		break;
	case CSS_PROP_FONT_WEIGHT:
		style->font_weight = value->keyword;
		break;
	case CSS_PROP_FONT_STYLE:
		style->font_italic = value->keyword;
		break;
	case CSS_PROP_FONT_FAMILY:
		cascade_families(style, value);
		break;
	case CSS_PROP_LINE_HEIGHT:
		if (value->kind == CSS_VALUE_NUMBER) {
			style->line_height.unit = CSS_UNIT_NUMBER;
			style->line_height.value = value->number;
		} else {
			style->line_height = cascade_length(engine, value, style->font_size);
			if (style->line_height.unit == CSS_UNIT_PERCENT) {
				style->line_height.unit = CSS_UNIT_PX;
				style->line_height.value = style->font_size * style->line_height.value / 100.0f + style->line_height.offset;
				style->line_height.offset = 0;
			}
		}

		break;
	case CSS_PROP_TEXT_ALIGN:
		style->text_align = value->keyword;
		break;
	case CSS_PROP_TEXT_INDENT:
		style->text_indent = cascade_length(engine, value, style->font_size);
		break;
	case CSS_PROP_DIRECTION:
		style->direction = value->keyword;
		break;
	case CSS_PROP_WRITING_MODE:
		style->writing_sideways = 0;
		style->writing_mode = value->keyword;
		if (value->keyword > CSS_WRITING_VERTICAL_RL) {
			style->writing_sideways = value->keyword - CSS_WRITING_VERTICAL_RL;
			style->writing_mode = CSS_WRITING_HORIZONTAL_TB;
		}
		break;
	case CSS_PROP_TEXT_ORIENTATION:
		style->text_orientation = value->keyword;
		break;
	case CSS_PROP_CONTAINER_TYPE:
		style->container_type = value->keyword;
		break;
	case CSS_PROP_BORDER_SPACING_X:
	case CSS_PROP_BORDER_SPACING_Y:
		style->border_spacing[property - CSS_PROP_BORDER_SPACING_X] = cascade_length(engine, value, style->font_size).value;
		break;
	case CSS_PROP_TRANSFORM_X:
	case CSS_PROP_TRANSFORM_Y:
		style->transform_translate[property - CSS_PROP_TRANSFORM_X] = cascade_length(engine, value, style->font_size);
		break;
	case CSS_PROP_OBJECT_FIT:
		style->object_fit = value->keyword;
		break;
	case CSS_PROP_OBJECT_POSITION_X:
	case CSS_PROP_OBJECT_POSITION_Y:
		style->object_position[property - CSS_PROP_OBJECT_POSITION_X] = cascade_length(engine, value, style->font_size);
		break;
	case CSS_PROP_ASPECT_RATIO:
		style->aspect_ratio = value->number;
		break;
	case CSS_PROP_ASPECT_AUTO:
		style->aspect_auto = value->number != 0.0f;
		break;
	case CSS_PROP_TRANSFORM_SCALE_X:
	case CSS_PROP_TRANSFORM_SCALE_Y:
		style->transform_scale[property - CSS_PROP_TRANSFORM_SCALE_X] = cascade_length(engine, value, style->font_size);
		break;
	case CSS_PROP_TRANSFORM_ORIGIN_X:
	case CSS_PROP_TRANSFORM_ORIGIN_Y:
		style->transform_origin[property - CSS_PROP_TRANSFORM_ORIGIN_X] = cascade_length(engine, value, style->font_size);
		break;
	case CSS_PROP_BORDER_COLLAPSE:
		style->border_collapse = value->keyword;
		break;
	case CSS_PROP_GRID_TEMPLATE_COLUMNS:
		cascade_tracks(engine, style, value, style->columns, &style->column_count, &style->column_repeat);
		style->column_names = cascade_line_names(value);
		break;
	case CSS_PROP_GRID_TEMPLATE_ROWS:
		cascade_tracks(engine, style, value, style->rows, &style->row_count, &style->row_repeat);
		style->row_names = cascade_line_names(value);
		break;
	case CSS_PROP_GRID_TEMPLATE_AREAS:
		/* The areas stay in the declaring sheet's arena. */
		style->areas = NULL;
		if (value->kind == CSS_VALUE_AREAS)
			style->areas = value->areas;
		break;
	case CSS_PROP_GRID_AUTO_FLOW:
		style->grid_auto_flow = value->keyword;
		break;
	case CSS_PROP_GRID_AUTO_COLUMNS:
		cascade_tracks(engine, style, value, style->auto_columns, &style->auto_column_count, NULL);
		break;
	case CSS_PROP_GRID_AUTO_ROWS:
		cascade_tracks(engine, style, value, style->auto_rows, &style->auto_row_count, NULL);
		break;
	case CSS_PROP_GRID_COLUMN_START:
	case CSS_PROP_GRID_COLUMN_END:
	case CSS_PROP_GRID_ROW_START:
	case CSS_PROP_GRID_ROW_END:
		/* A line (0 for auto) or a span, at the start or the end of the column or the row. */
		place = &style->grid_column;
		if (property == CSS_PROP_GRID_ROW_START || property == CSS_PROP_GRID_ROW_END)
			place = &style->grid_row;
		line = 0;
		span = 0;
		name = NULL;
		if (value->kind == CSS_VALUE_GRID_LINE) {
			line = (int)value->number;
			span = value->keyword;
			name = value->ident;
		}

		/* The start's line, span and name, or the end's. */
		if (property == CSS_PROP_GRID_COLUMN_START || property == CSS_PROP_GRID_ROW_START) {
			place->start = line;
			place->start_span = span;
			place->start_name = name;
		} else {
			place->end = line;
			place->end_span = span;
			place->end_name = name;
		}

		/* The place is set. */
		break;
	case CSS_PROP_CURSOR:
		style->cursor = value->keyword;
		break;
	case CSS_PROP_TEXT_TRANSFORM:
		/* Retain the specified keyword without changing the underlying DOM text. */
		style->text_transform = value->keyword;
		break;
	case CSS_PROP_WHITE_SPACE:
		style->white_space = value->keyword;
		break;
	case CSS_PROP_TEXT_DECORATION_LINE:
		style->text_decoration_line = value->keyword;
		style->underline = (value->keyword & CSS_DECORATION_UNDERLINE) != 0;
		break;
	case CSS_PROP_TEXT_DECORATION_THICKNESS:
		if (value->kind == CSS_VALUE_LENGTH) {
			style->text_decoration_thickness_kind = 2;
			style->text_decoration_thickness = cascade_length(engine, value, style->font_size);
		} else {
			style->text_decoration_thickness_kind = value->keyword;
		}
		break;
	case CSS_PROP_TEXT_DECORATION_STYLE:
		style->text_decoration_style = value->keyword;
		break;
	case CSS_PROP_TEXT_DECORATION_SKIP_INK:
		style->text_decoration_skip_ink = value->keyword;
		break;
	case CSS_PROP_TEXT_DECORATION_COLOR:
		style->text_decoration_current_color = value->color == CSS_CURRENT_COLOR;
		style->text_decoration_color = value->color;
		break;
	case CSS_PROP_LIST_STYLE_TYPE:
		style->list_style = value->keyword;
		break;
	case CSS_PROP_BOX_SIZING:
		style->box_sizing = value->keyword;
		break;
	case CSS_PROP_FLEX_DIRECTION:
		style->flex_direction = value->keyword;
		break;
	case CSS_PROP_FLEX_WRAP:
		style->flex_wrap = value->keyword;
		break;
	case CSS_PROP_JUSTIFY_CONTENT:
		cascade_align(style, 1u, &style->justify_content, value->keyword);
		break;
	case CSS_PROP_JUSTIFY_ITEMS:
		cascade_align(style, 0x200u, &style->justify_items, value->keyword);
		break;
	case CSS_PROP_ALIGN_ITEMS:
		cascade_align(style, 4u, &style->align_items, value->keyword);
		break;
	case CSS_PROP_ALIGN_CONTENT:
		cascade_align(style, 2u, &style->align_content, value->keyword);
		break;
	case CSS_PROP_ALIGN_SELF:
		cascade_align(style, 8u, &style->align_self, value->keyword);
		break;
	case CSS_PROP_JUSTIFY_SELF:
		cascade_align(style, 0x100u, &style->justify_self, value->keyword);
		break;
	case CSS_PROP_ROW_GAP:
		style->row_gap = cascade_length(engine, value, style->font_size);
		break;
	case CSS_PROP_COLUMN_GAP:
		style->column_gap = cascade_length(engine, value, style->font_size);
		break;
	case CSS_PROP_FLEX_GROW:
		style->flex_grow = value->number;
		break;
	case CSS_PROP_FLEX_SHRINK:
		style->flex_shrink = value->number;
		break;
	case CSS_PROP_FLEX_BASIS:
		style->flex_basis = cascade_flex_basis(engine, value, style->font_size);
		break;
	case CSS_PROP_ORDER:
		style->order = (int)value->number;
		break;
	case CSS_PROP_RADIUS_TOP_LEFT_X:
	case CSS_PROP_RADIUS_TOP_LEFT_Y:
	case CSS_PROP_RADIUS_TOP_RIGHT_X:
	case CSS_PROP_RADIUS_TOP_RIGHT_Y:
	case CSS_PROP_RADIUS_BOTTOM_RIGHT_X:
	case CSS_PROP_RADIUS_BOTTOM_RIGHT_Y:
	case CSS_PROP_RADIUS_BOTTOM_LEFT_X:
	case CSS_PROP_RADIUS_BOTTOM_LEFT_Y:
		/* The corner is every second property, horizontal before vertical. */
		style->radius[(property - CSS_PROP_RADIUS_TOP_LEFT_X) / 2][(property - CSS_PROP_RADIUS_TOP_LEFT_X) % 2] =
		    cascade_length(engine, value, style->font_size);
		break;
	case CSS_PROP_OPACITY:
		style->opacity = value->number;
		break;
	case CSS_PROP_BOX_SHADOW:
		cascade_shadows(engine, style, value);
		break;
	case CSS_PROP_TEXT_SHADOW:
		cascade_text_shadow(engine, style, value);
		break;
	case CSS_PROP_OUTLINE_WIDTH:
		style->outline_width = cascade_length(engine, value, style->font_size).value;
		break;
	case CSS_PROP_OUTLINE_STYLE:
		style->outline_style = value->keyword;
		break;
	case CSS_PROP_OUTLINE_COLOR:
		style->outline_color = value->color;
		break;
	case CSS_PROP_OUTLINE_OFFSET:
		style->outline_offset = cascade_length(engine, value, style->font_size).value;
		break;
	case CSS_PROP_CLIP_PATH:
		/* An inset's four lengths, or no clip. */
		style->clip_inset = 0;
		if (value->kind == CSS_VALUE_INSET && value->inset != NULL) {
			style->clip_inset = 1;
			for (side = 0; side < 4; side++)
				style->clip[side] = cascade_length(engine, &value->inset->lengths[side], style->font_size);
		}

		/* The clip is set. */
		break;
	case CSS_PROP_CLIP:
		/* rect()'s four edges, or no clip. */
		style->clip_rect = 0;
		if (value->kind == CSS_VALUE_INSET && value->inset != NULL) {
			style->clip_rect = 1;
			for (side = 0; side < 4; side++)
				style->clip_edges[side] = cascade_length(engine, &value->inset->lengths[side], style->font_size);
		}
		break;
	case CSS_PROP_VERTICAL_ALIGN:
		/* A keyword, or a length that raises the box by it. */
		if (value->kind == CSS_VALUE_KEYWORD) {
			style->vertical_align = value->keyword;
		} else {
			style->vertical_align = CSS_VALIGN_LENGTH;
			style->vertical_offset = cascade_length(engine, value, style->font_size);
		}

		/* The alignment is set. */
		break;
	case CSS_PROP_CONTENT:
		/* A list of items, or none (normal and none). */
		style->content_kind = CSS_CONTENT_NONE;
		style->content = NULL;
		if (value->kind == CSS_VALUE_CONTENT) {
			style->content_kind = CSS_CONTENT_LIST;
			style->content = value->content;
		}

		/* The content is set. */
		break;
	default:
		break;
	}
}

/* Copies one property's computed value from another style. */
static void
cascade_inherit(
	struct css_style *style,
	const struct css_style *parent,
	int property)
{
	/* The property's field or fields. */
	switch (property) {
	case CSS_PROP_DISPLAY:
		style->display = parent->display;
		break;
	case CSS_PROP_POSITION:
		style->position = parent->position;
		break;
	case CSS_PROP_FLOAT:
		style->float_side = parent->float_side;
		break;
	case CSS_PROP_CLEAR:
		style->clear = parent->clear;
		break;
	case CSS_PROP_OVERFLOW:
		style->overflow_x = parent->overflow_x;
		style->overflow_y = parent->overflow_y;
		break;
	case CSS_PROP_OVERFLOW_X:
		style->overflow_x = parent->overflow_x;
		break;
	case CSS_PROP_OVERFLOW_Y:
		style->overflow_y = parent->overflow_y;
		break;
	case CSS_PROP_VISIBILITY:
		style->visibility = parent->visibility;
		break;
	case CSS_PROP_BOX_SIZING:
		style->box_sizing = parent->box_sizing;
		break;
	case CSS_PROP_CONTENT:
		style->content_kind = parent->content_kind;
		style->content = parent->content;
		break;
	case CSS_PROP_FLEX_DIRECTION:
		style->flex_direction = parent->flex_direction;
		break;
	case CSS_PROP_FLEX_WRAP:
		style->flex_wrap = parent->flex_wrap;
		break;
	case CSS_PROP_JUSTIFY_CONTENT:
		style->justify_content = parent->justify_content;
		style->flex_keywords = (style->flex_keywords & ~1u) | (parent->flex_keywords & 1u);
		break;
	case CSS_PROP_JUSTIFY_ITEMS:
		style->justify_items = parent->justify_items;
		style->flex_keywords = (style->flex_keywords & ~0u) | (parent->flex_keywords & 0u);
		break;
	case CSS_PROP_ALIGN_ITEMS:
		style->align_items = parent->align_items;
		style->flex_keywords = (style->flex_keywords & ~4u) | (parent->flex_keywords & 4u);
		break;
	case CSS_PROP_ALIGN_CONTENT:
		style->align_content = parent->align_content;
		style->flex_keywords = (style->flex_keywords & ~2u) | (parent->flex_keywords & 2u);
		break;
	case CSS_PROP_ALIGN_SELF:
		style->align_self = parent->align_self;
		style->flex_keywords = (style->flex_keywords & ~8u) | (parent->flex_keywords & 8u);
		break;
	case CSS_PROP_JUSTIFY_SELF:
		style->justify_self = parent->justify_self;
		style->flex_keywords = (style->flex_keywords & ~0u) | (parent->flex_keywords & 0u);
		break;
	case CSS_PROP_ROW_GAP:
		style->row_gap = parent->row_gap;
		break;
	case CSS_PROP_COLUMN_GAP:
		style->column_gap = parent->column_gap;
		break;
	case CSS_PROP_FLEX_GROW:
		style->flex_grow = parent->flex_grow;
		break;
	case CSS_PROP_FLEX_SHRINK:
		style->flex_shrink = parent->flex_shrink;
		break;
	case CSS_PROP_FLEX_BASIS:
		style->flex_basis = parent->flex_basis;
		break;
	case CSS_PROP_ORDER:
		style->order = parent->order;
		break;
	case CSS_PROP_VERTICAL_ALIGN:
		style->vertical_align = parent->vertical_align;
		style->vertical_offset = parent->vertical_offset;
		break;
	case CSS_PROP_RADIUS_TOP_LEFT_X:
	case CSS_PROP_RADIUS_TOP_LEFT_Y:
	case CSS_PROP_RADIUS_TOP_RIGHT_X:
	case CSS_PROP_RADIUS_TOP_RIGHT_Y:
	case CSS_PROP_RADIUS_BOTTOM_RIGHT_X:
	case CSS_PROP_RADIUS_BOTTOM_RIGHT_Y:
	case CSS_PROP_RADIUS_BOTTOM_LEFT_X:
	case CSS_PROP_RADIUS_BOTTOM_LEFT_Y:
		style->radius[(property - CSS_PROP_RADIUS_TOP_LEFT_X) / 2][(property - CSS_PROP_RADIUS_TOP_LEFT_X) % 2] =
		    parent->radius[(property - CSS_PROP_RADIUS_TOP_LEFT_X) / 2][(property - CSS_PROP_RADIUS_TOP_LEFT_X) % 2];
		break;
	case CSS_PROP_OPACITY:
		style->opacity = parent->opacity;
		break;
	case CSS_PROP_BOX_SHADOW:
		memcpy(style->shadows, parent->shadows, sizeof(style->shadows));
		style->shadow_count = parent->shadow_count;
		break;
	case CSS_PROP_TEXT_SHADOW:
		style->text_shadow = parent->text_shadow;
		style->text_shadow_set = parent->text_shadow_set;
		break;
	case CSS_PROP_OUTLINE_WIDTH:
		style->outline_width = parent->outline_width;
		break;
	case CSS_PROP_OUTLINE_STYLE:
		style->outline_style = parent->outline_style;
		break;
	case CSS_PROP_OUTLINE_COLOR:
		style->outline_color = parent->outline_color;
		break;
	case CSS_PROP_OUTLINE_OFFSET:
		style->outline_offset = parent->outline_offset;
		break;
	case CSS_PROP_CLIP_PATH:
		style->clip_inset = parent->clip_inset;
		memcpy(style->clip, parent->clip, sizeof(style->clip));
		break;
	case CSS_PROP_CLIP:
		style->clip_rect = parent->clip_rect;
		memcpy(style->clip_edges, parent->clip_edges, sizeof(style->clip_edges));
		break;
	case CSS_PROP_WIDTH:
		style->width = parent->width;
		break;
	case CSS_PROP_HEIGHT:
		style->height = parent->height;
		break;
	case CSS_PROP_MIN_WIDTH:
		style->min_width = parent->min_width;
		break;
	case CSS_PROP_MAX_WIDTH:
		style->max_width = parent->max_width;
		break;
	case CSS_PROP_MIN_HEIGHT:
		style->min_height = parent->min_height;
		break;
	case CSS_PROP_MAX_HEIGHT:
		style->max_height = parent->max_height;
		break;
	case CSS_PROP_MARGIN_TOP:
	case CSS_PROP_MARGIN_RIGHT:
	case CSS_PROP_MARGIN_BOTTOM:
	case CSS_PROP_MARGIN_LEFT:
		style->margin[property - CSS_PROP_MARGIN_TOP] = parent->margin[property - CSS_PROP_MARGIN_TOP];
		break;
	case CSS_PROP_PADDING_TOP:
	case CSS_PROP_PADDING_RIGHT:
	case CSS_PROP_PADDING_BOTTOM:
	case CSS_PROP_PADDING_LEFT:
		style->padding[property - CSS_PROP_PADDING_TOP] = parent->padding[property - CSS_PROP_PADDING_TOP];
		break;
	case CSS_PROP_TOP:
	case CSS_PROP_RIGHT:
	case CSS_PROP_BOTTOM:
	case CSS_PROP_LEFT:
		style->offset[property - CSS_PROP_TOP] = parent->offset[property - CSS_PROP_TOP];
		break;
	case CSS_PROP_Z_INDEX:
		style->z_index = parent->z_index;
		style->z_index_auto = parent->z_index_auto;
		break;
	case CSS_PROP_BORDER_TOP_WIDTH:
	case CSS_PROP_BORDER_RIGHT_WIDTH:
	case CSS_PROP_BORDER_BOTTOM_WIDTH:
	case CSS_PROP_BORDER_LEFT_WIDTH:
		style->border_width[property - CSS_PROP_BORDER_TOP_WIDTH] = parent->border_width[property - CSS_PROP_BORDER_TOP_WIDTH];
		break;
	case CSS_PROP_BORDER_TOP_STYLE:
	case CSS_PROP_BORDER_RIGHT_STYLE:
	case CSS_PROP_BORDER_BOTTOM_STYLE:
	case CSS_PROP_BORDER_LEFT_STYLE:
		style->border_style[property - CSS_PROP_BORDER_TOP_STYLE] = parent->border_style[property - CSS_PROP_BORDER_TOP_STYLE];
		break;
	case CSS_PROP_BORDER_TOP_COLOR:
	case CSS_PROP_BORDER_RIGHT_COLOR:
	case CSS_PROP_BORDER_BOTTOM_COLOR:
	case CSS_PROP_BORDER_LEFT_COLOR:
		style->border_color[property - CSS_PROP_BORDER_TOP_COLOR] = parent->border_color[property - CSS_PROP_BORDER_TOP_COLOR];
		break;
	case CSS_PROP_COLOR:
		style->color = parent->color;
		break;
	case CSS_PROP_BACKGROUND_COLOR:
		style->background_color = parent->background_color;
		break;
	case CSS_PROP_BACKGROUND_IMAGE:
		style->background_image = parent->background_image;
		break;
	case CSS_PROP_BACKGROUND_REPEAT:
		style->background_repeat = parent->background_repeat;
		break;
	case CSS_PROP_BACKGROUND_ATTACHMENT:
		style->background_attachment = parent->background_attachment;
		break;
	case CSS_PROP_BACKGROUND_POSITION_X:
	case CSS_PROP_BACKGROUND_POSITION_Y:
		style->background_position[property - CSS_PROP_BACKGROUND_POSITION_X] = parent->background_position[property - CSS_PROP_BACKGROUND_POSITION_X];
		break;
	case CSS_PROP_BACKGROUND_SIZE_WIDTH:
		style->background_size_keyword = parent->background_size_keyword;
		style->background_size[0] = parent->background_size[0];
		break;
	case CSS_PROP_BACKGROUND_SIZE_HEIGHT:
		style->background_size[1] = parent->background_size[1];
		break;
	case CSS_PROP_FONT_SIZE:
		style->font_size = parent->font_size;
		style->font_size_keyword = parent->font_size_keyword;
		break;
	case CSS_PROP_FONT_WEIGHT:
		style->font_weight = parent->font_weight;
		break;
	case CSS_PROP_FONT_STYLE:
		style->font_italic = parent->font_italic;
		break;
	case CSS_PROP_FONT_FAMILY:
		style->generic_family = parent->generic_family;
		memcpy(style->families, parent->families, sizeof(style->families));
		style->family_count = parent->family_count;
		break;
	case CSS_PROP_LINE_HEIGHT:
		style->line_height = parent->line_height;
		break;
	case CSS_PROP_TEXT_ALIGN:
		style->text_align = parent->text_align;
		break;
	case CSS_PROP_TEXT_INDENT:
		style->text_indent = parent->text_indent;
		break;
	case CSS_PROP_DIRECTION:
		style->direction = parent->direction;
		break;
	case CSS_PROP_WRITING_MODE:
		style->writing_mode = parent->writing_mode;
		style->writing_sideways = parent->writing_sideways;
		break;
	case CSS_PROP_TEXT_ORIENTATION:
		style->text_orientation = parent->text_orientation;
		break;
	case CSS_PROP_CONTAINER_TYPE:
		style->container_type = parent->container_type;
		break;
	case CSS_PROP_TRANSFORM_X:
	case CSS_PROP_TRANSFORM_Y:
		style->transform_translate[property - CSS_PROP_TRANSFORM_X] = parent->transform_translate[property - CSS_PROP_TRANSFORM_X];
		break;
	case CSS_PROP_OBJECT_FIT:
		style->object_fit = parent->object_fit;
		break;
	case CSS_PROP_OBJECT_POSITION_X:
	case CSS_PROP_OBJECT_POSITION_Y:
		style->object_position[property - CSS_PROP_OBJECT_POSITION_X] = parent->object_position[property - CSS_PROP_OBJECT_POSITION_X];
		break;
	case CSS_PROP_ASPECT_RATIO:
		style->aspect_ratio = parent->aspect_ratio;
		break;
	case CSS_PROP_ASPECT_AUTO:
		style->aspect_auto = parent->aspect_auto;
		break;
	case CSS_PROP_TRANSFORM_SCALE_X:
	case CSS_PROP_TRANSFORM_SCALE_Y:
		style->transform_scale[property - CSS_PROP_TRANSFORM_SCALE_X] = parent->transform_scale[property - CSS_PROP_TRANSFORM_SCALE_X];
		break;
	case CSS_PROP_TRANSFORM_ORIGIN_X:
	case CSS_PROP_TRANSFORM_ORIGIN_Y:
		style->transform_origin[property - CSS_PROP_TRANSFORM_ORIGIN_X] = parent->transform_origin[property - CSS_PROP_TRANSFORM_ORIGIN_X];
		break;
	case CSS_PROP_BORDER_SPACING_X:
	case CSS_PROP_BORDER_SPACING_Y:
		style->border_spacing[property - CSS_PROP_BORDER_SPACING_X] = parent->border_spacing[property - CSS_PROP_BORDER_SPACING_X];
		break;
	case CSS_PROP_BORDER_COLLAPSE:
		style->border_collapse = parent->border_collapse;
		break;
	case CSS_PROP_GRID_TEMPLATE_COLUMNS:
		memcpy(style->columns, parent->columns, sizeof(style->columns));
		style->column_count = parent->column_count;
		style->column_repeat = parent->column_repeat;
		style->column_names = parent->column_names;
		break;
	case CSS_PROP_GRID_TEMPLATE_ROWS:
		memcpy(style->rows, parent->rows, sizeof(style->rows));
		style->row_count = parent->row_count;
		style->row_repeat = parent->row_repeat;
		style->row_names = parent->row_names;
		break;
	case CSS_PROP_GRID_TEMPLATE_AREAS:
		style->areas = parent->areas;
		break;
	case CSS_PROP_GRID_AUTO_FLOW:
		style->grid_auto_flow = parent->grid_auto_flow;
		break;
	case CSS_PROP_GRID_AUTO_COLUMNS:
		memcpy(style->auto_columns, parent->auto_columns, sizeof(style->auto_columns));
		style->auto_column_count = parent->auto_column_count;
		break;
	case CSS_PROP_GRID_AUTO_ROWS:
		memcpy(style->auto_rows, parent->auto_rows, sizeof(style->auto_rows));
		style->auto_row_count = parent->auto_row_count;
		break;
	case CSS_PROP_GRID_COLUMN_START:
	case CSS_PROP_GRID_COLUMN_END:
		style->grid_column = parent->grid_column;
		break;
	case CSS_PROP_GRID_ROW_START:
	case CSS_PROP_GRID_ROW_END:
		style->grid_row = parent->grid_row;
		break;
	case CSS_PROP_CURSOR:
		style->cursor = parent->cursor;
		break;
	case CSS_PROP_TEXT_TRANSFORM:
		/* Explicit inheritance and initial/unset use the same native computed field. */
		style->text_transform = parent->text_transform;
		break;
	case CSS_PROP_WHITE_SPACE:
		style->white_space = parent->white_space;
		break;
	case CSS_PROP_TEXT_DECORATION_LINE:
		style->text_decoration_line = parent->text_decoration_line;
		style->underline = parent->underline;
		break;
	case CSS_PROP_TEXT_DECORATION_THICKNESS:
		style->text_decoration_thickness_kind = parent->text_decoration_thickness_kind;
		style->text_decoration_thickness = parent->text_decoration_thickness;
		break;
	case CSS_PROP_TEXT_DECORATION_STYLE:
		style->text_decoration_style = parent->text_decoration_style;
		break;
	case CSS_PROP_TEXT_DECORATION_SKIP_INK:
		style->text_decoration_skip_ink = parent->text_decoration_skip_ink;
		break;
	case CSS_PROP_TEXT_DECORATION_COLOR:
		style->text_decoration_current_color = parent->text_decoration_current_color;
		style->text_decoration_color = parent->text_decoration_color;
		break;
	case CSS_PROP_LIST_STYLE_TYPE:
		style->list_style = parent->list_style;
		break;
	default:
		break;
	}
}

/* Stores an alignment keyword; flex-start and flex-end are start and end that remember how they were written. */
static void
cascade_align(
	struct css_style *style,
	unsigned bit,
	int *field,
	int keyword)
{
	style->flex_keywords &= ~(bit | (bit << 4));
	if ((keyword & 0x1000) != 0) {
		keyword &= ~0x1000;
		style->flex_keywords |= bit << 4;
	}
	if (keyword == CSS_ALIGN_FLEX_START) {
		keyword = CSS_ALIGN_START;
		style->flex_keywords |= bit;
	} else if (keyword == CSS_ALIGN_FLEX_END) {
		keyword = CSS_ALIGN_END;
		style->flex_keywords |= bit;
	}
	*field = keyword;
}

/*
 * Converts a declared grid template into computed tracks:
 * lengths in pixels or percentages, fr shares, auto; none is no tracks.
 */
static void
cascade_tracks(
	struct css_engine *engine,
	struct css_style *style,
	const struct css_value *value,
	struct css_track *tracks,
	int *count,
	struct css_track_repeat *repeat)
{
	const struct css_declared_track *declared;
	size_t index;

	/* Anything but a list is no template. */
	*count = 0;
	if (repeat != NULL)
		memset(repeat, 0, sizeof(*repeat));

	/* Only parsed track lists can supply computed tracks. */
	if (value->kind != CSS_VALUE_TRACKS || value->tracks == NULL)
		return;

	/* Copies the bounded auto-repeat descriptor before converting its pattern. */
	if (repeat != NULL && value->tracks->repeat_length > 0 &&
	    value->tracks->repeat_at + value->tracks->repeat_length <= CSS_TRACKS) {
		repeat->length = value->tracks->repeat_length;
		repeat->at = value->tracks->repeat_at;
		repeat->fit = value->tracks->repeat_fit;
	}

	/* Each track. */
	for (index = 0; index < value->tracks->count && index < CSS_TRACKS; index++) {
		/* Initializes one complete computed scalar track before resolving its breadths. */
		declared = &value->tracks->tracks[index];
		memset(&tracks[index], 0, sizeof(tracks[index]));
		tracks[index].kind = declared->kind;

		/* Preserve the explicit lower-bound rule as its individual lengths resolve below. */
		tracks[index].minimum_rule = declared->minimum_rule;
		tracks[index].maximum_rule = declared->maximum_rule;
		tracks[index].minimum.unit = CSS_UNIT_AUTO;

		/* Its size: an fr share, or a nonnegative resolved length. */
		if (declared->kind == CSS_TRACK_FR)
			tracks[index].fr = declared->size.number;

		/* Resolves the independent maximum without applying its used lower floor. */
		if (declared->kind == CSS_TRACK_LENGTH || declared->maximum_rule == CSS_TRACK_MAXIMUM_FIT_CONTENT) {
			tracks[index].size = cascade_length(engine, &declared->size, style->font_size);

			/* Negative computed lengths are range-clamped while intrinsic rules stay separate. */
			if (tracks[index].size.unit == CSS_UNIT_PX && tracks[index].size.value < 0)
				tracks[index].size.value = 0;

			/* Pure negative percentage fit limits have an immediately known nonnegative range. */
			if (declared->maximum_rule == CSS_TRACK_MAXIMUM_FIT_CONTENT &&
			    tracks[index].size.unit == CSS_UNIT_PERCENT &&
			    tracks[index].size.offset == 0 && tracks[index].size.value < 0)
				tracks[index].size.value = 0;

			/* Computed serialization retains the upper bound before native lower-floor normalization. */
			tracks[index].computed_maximum = tracks[index].size;
		}

		/* Its minimum, when minmax() gave a length, has the same range. */
		if (declared->minimum_kind == CSS_TRACK_LENGTH) {
			tracks[index].minimum = cascade_length(engine, &declared->minimum, style->font_size);

			/* The explicit lower length has the same nonnegative computed range. */
			if (tracks[index].minimum.unit == CSS_UNIT_PX && tracks[index].minimum.value < 0)
				tracks[index].minimum.value = 0;

			/* Existing native consumers retain their absolute lower-floor normalization. */
			if (declared->kind == CSS_TRACK_LENGTH &&
			    tracks[index].size.unit == CSS_UNIT_PX &&
			    tracks[index].minimum.unit == CSS_UNIT_PX &&
			    tracks[index].size.value < tracks[index].minimum.value)
				tracks[index].size = tracks[index].minimum;
		}
	}

	/* The number of tracks. */
	*count = (int)index;
}

/* The line names a declared grid template gives its lines (in the sheet's arena), or NULL when it names none. */
static const struct css_line_names *
cascade_line_names(
	const struct css_value *value)
{
	/* Only a parsed track list with a name has names. */
	if (value->kind != CSS_VALUE_TRACKS || value->tracks == NULL)
		return NULL;
	if (!value->tracks->named)
		return NULL;

	/* Succeeded: the list's lines. */
	return value->tracks->names;
}

/*
 * Converts a declared box-shadow list into the style's shadows in pixels;
 * a percentage, which a shadow's length cannot be, is zero.
 */
static void
cascade_shadows(
	struct css_engine *engine,
	struct css_style *style,
	const struct css_value *value)
{
	const struct css_declared_shadow *declared;
	struct css_shadow *shadow;
	struct css_length length;
	float lengths[4];
	size_t index;
	size_t part;

	/* Anything but a list is no shadow. */
	style->shadow_count = 0;
	if (value->kind != CSS_VALUE_SHADOWS || value->shadows == NULL)
		return;

	/* Each shadow's lengths in pixels, its color and whether it is inset. */
	for (index = 0; index < value->shadows->count && index < CSS_SHADOWS; index++) {
		declared = &value->shadows->shadows[index];
		for (part = 0; part < 4U; part++) {
			length = cascade_length(engine, &declared->lengths[part], style->font_size);
			lengths[part] = 0;
			if (length.unit == CSS_UNIT_PX)
				lengths[part] = length.value;
		}

		/* The shadow. */
		shadow = &style->shadows[index];
		shadow->x = lengths[0];
		shadow->y = lengths[1];
		shadow->blur = lengths[2];
		shadow->spread = lengths[3];
		shadow->color = declared->color;
		shadow->inset = declared->inset;
		style->shadow_count++;
	}
}

/* Computes the single sharp text shadow supported by this pass. */
static void
cascade_text_shadow(
	struct css_engine *engine,
	struct css_style *style,
	const struct css_value *value)
{
	const struct css_declared_shadow *declared;
	struct css_length length;

	style->text_shadow_set = 0;
	if (value->kind != CSS_VALUE_SHADOWS || value->shadows == NULL || value->shadows->count == 0)
		return;
	declared = &value->shadows->shadows[0];
	length = cascade_length(engine, &declared->lengths[0], style->font_size);
	style->text_shadow.x = length.unit == CSS_UNIT_PX ? length.value : 0.0f;
	length = cascade_length(engine, &declared->lengths[1], style->font_size);
	style->text_shadow.y = length.unit == CSS_UNIT_PX ? length.value : 0.0f;
	style->text_shadow.color = declared->color;
	style->text_shadow_set = 1;
}

/* Clamps an absolute computed flex basis without losing percentage dependence. */
static struct css_length
cascade_flex_basis(
	struct css_engine *engine,
	const struct css_value *value,
	float font_size)
{
	struct css_length basis;

	/* Resolves absolute units while retaining the calc percentage type. */
	basis = cascade_length(engine, value, font_size);

	/* Absolute math results obey the nonnegative computed range. */
	if (basis.unit == CSS_UNIT_PX) {
		/* An absolute negative cannot be represented by this property. */
		if (basis.value < 0)
			basis.value = 0;
	}

	/* A pure percentage can be range-clamped without resolving its basis. */
	if (basis.unit == CSS_UNIT_PERCENT && basis.offset == 0) {
		/* No pixel term remains that could offset the negative percentage. */
		if (basis.value < 0)
			basis.value = 0;
	}

	/* Succeeded: mixed expressions defer their range check until use. */
	return basis;
}

/* Converts a declared length to a computed one: pixels, a percentage or a keyword. */
static struct css_length
cascade_length(
	struct css_engine *engine,
	const struct css_value *value,
	float font_size)
{
	struct css_length length;
	float container_width;
	float container_height;

	/* Keywords keep their unit. */
	length.value = 0;
	length.unit = CSS_UNIT_PX;
	length.offset = 0;
	if (value->kind == CSS_VALUE_KEYWORD) {
		length.unit = value->keyword;
		return length;
	}

	/* A calculation. */
	if (value->unit == CSS_DUNIT_CALC && value->calc != NULL) {
		length = cascade_calc(engine, value->calc, font_size);
		return length;
	}

	/* Each unit's size in pixels. */
	switch (value->unit) {
	case CSS_DUNIT_PX:
		length.value = value->number;
		break;
	case CSS_DUNIT_EM:
		length.value = value->number * font_size;
		break;
	case CSS_DUNIT_REM:
		length.value = value->number * engine->root_font_size;
		break;
	case CSS_DUNIT_EX:
		length.value = value->number * font_size * 0.5f;
		break;
	case CSS_DUNIT_PERCENT:
		length.value = value->number;
		length.unit = CSS_UNIT_PERCENT;
		break;
	case CSS_DUNIT_PT:
		length.value = value->number * 96.0f / 72.0f;
		break;
	case CSS_DUNIT_PC:
		length.value = value->number * 16.0f;
		break;
	case CSS_DUNIT_IN:
		length.value = value->number * 96.0f;
		break;
	case CSS_DUNIT_CM:
		length.value = value->number * 96.0f / 2.54f;
		break;
	case CSS_DUNIT_MM:
		length.value = value->number * 96.0f / 25.4f;
		break;
	case CSS_DUNIT_VW:
		length.value = value->number * engine->viewport_width / 100.0f;
		break;
	case CSS_DUNIT_CQW:
		cascade_container(engine, &container_width, &container_height);
		length.value = value->number * container_width / 100.0f;
		break;
	case CSS_DUNIT_CQH:
		cascade_container(engine, &container_width, &container_height);
		length.value = value->number * container_height / 100.0f;
		break;
	case CSS_DUNIT_VH:
		length.value = value->number * engine->viewport_height / 100.0f;
		break;
	case CSS_DUNIT_VMIN:
		length.value = value->number * engine->viewport_width / 100.0f;
		if (engine->viewport_height < engine->viewport_width)
			length.value = value->number * engine->viewport_height / 100.0f;
		break;
	case CSS_DUNIT_VMAX:
		length.value = value->number * engine->viewport_width / 100.0f;
		if (engine->viewport_height > engine->viewport_width)
			length.value = value->number * engine->viewport_height / 100.0f;
		break;
	default:
		break;
	}

	/* Reports the computed length. */
	return length;
}

/* Computes a font size from its declared value and the parent's size. */
static float
cascade_font_size(
	struct css_engine *engine,
	const struct css_value *value,
	float parent_size)
{
	struct css_length length;

	/* em and percentages measure the parent's size. */
	if (value->kind == CSS_VALUE_LENGTH && value->unit == CSS_DUNIT_PERCENT)
		return parent_size * value->number / 100.0f;

	/* So do a calculation's. */
	if (value->kind == CSS_VALUE_LENGTH && value->unit == CSS_DUNIT_CALC && value->calc != NULL) {
		length = cascade_calc(engine, value->calc, parent_size);
		if (length.unit == CSS_UNIT_PERCENT)
			return parent_size * length.value / 100.0f + length.offset;
		return length.value;
	}

	/* A keyword is its size at the default size. */
	if (value->kind == CSS_VALUE_LENGTH && value->unit == CSS_DUNIT_FONT_KEYWORD)
		return value->number;

	/* Resolves every other length and accepts only a pixel result. */
	length = cascade_length(engine, value, parent_size);
	if (length.unit != CSS_UNIT_PX)
		return parent_size;

	/* Reports the size in pixels. */
	return length.value;
}

/* Records a font-family list: the names, and the generic family the list ends in. */
static void
cascade_families(
	struct css_style *style,
	const struct css_value *value)
{
	int index;
	int monospace;
	int sans_serif;
	int serif;

	/* Keeps the names; the last generic name in the list picks the generic family. */
	style->family_count = value->family_count;
	for (index = 0; index < value->family_count; index++) {
		style->families[index] = value->families[index];
		monospace = vm_string_equal_ascii(value->families[index], "monospace");
		sans_serif = vm_string_equal_ascii(value->families[index], "sans-serif");
		serif = vm_string_equal_ascii(value->families[index], "serif");
		if (monospace) {
			style->generic_family = CSS_FAMILY_MONOSPACE;
		} else if (sans_serif) {
			style->generic_family = CSS_FAMILY_SANS_SERIF;
		} else if (serif) {
			style->generic_family = CSS_FAMILY_SERIF;
		}
	}
}

/* Tells whether a declared font size keeps a size on the keyword scale. */
static int
cascade_font_size_keyword(
	const struct css_value *value,
	int parent_keyword)
{
	/* A keyword is on the scale by definition. */
	if (value->kind == CSS_VALUE_LENGTH && value->unit == CSS_DUNIT_FONT_KEYWORD)
		return 1;

	/* An em or a percentage keeps the parent's scale. */
	if (value->kind == CSS_VALUE_LENGTH && value->unit == CSS_DUNIT_EM)
		return parent_keyword;
	if (value->kind == CSS_VALUE_LENGTH && value->unit == CSS_DUNIT_PERCENT)
		return parent_keyword;

	/* Any other length is absolute. */
	return 0;
}

/* Tells whether a style's font family is the monospace family and nothing else. */
static int
cascade_monospace_only(
	const struct css_style *style)
{
	int monospace;

	/* Only a list of one name can be monospace alone. */
	if (style->family_count != 1)
		return 0;

	/* That name must be the generic monospace. */
	monospace = vm_string_equal_ascii(style->families[0], "monospace");
	if (!monospace)
		return 0;

	/* The family is monospace alone. */
	return 1;
}

/*
 * Rescales a font size on the keyword scale when the family becomes, or
 * stops being, the monospace family alone.
 *
 * Chromium's default monospace size is 13 pixels against 16 for the other
 * families, and a size that follows the keywords follows that default: a
 * keyword declared here was sized for the other families, and an inherited
 * or relative size for the parent's family.
 */
static void
cascade_monospace_size(
	struct css_style *style,
	const struct css_style *parent,
	const struct css_declaration *font_size)
{
	int reference_monospace;
	int monospace;

	/* An absolute size is not rescaled. */
	if (!style->font_size_keyword)
		return;

	/* The family the size was measured for: the parent's, or the others' for a keyword declared here. */
	reference_monospace = 0;
	if (parent != NULL)
		reference_monospace = cascade_monospace_only(parent);
	if (font_size != NULL &&
	    font_size->value.kind == CSS_VALUE_LENGTH &&
	    font_size->value.unit == CSS_DUNIT_FONT_KEYWORD)
		reference_monospace = 0;

	/* Nothing changes while the family stays on the same side. */
	monospace = cascade_monospace_only(style);
	if (monospace == reference_monospace)
		return;

	/* Scales the size by the ratio of the two defaults. */
	if (monospace) {
		style->font_size = style->font_size * CASCADE_MONOSPACE_FONT_SIZE / CASCADE_DEFAULT_FONT_SIZE;
	} else {
		style->font_size = style->font_size * CASCADE_DEFAULT_FONT_SIZE / CASCADE_MONOSPACE_FONT_SIZE;
	}
}
