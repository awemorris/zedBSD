/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Computes styles in initial child Documents without a page host. */

#include "bind/internal.h"
#include "css/css.h"

#include <errno.h>
#include <stdlib.h>

/* One Window owns this C cache; its sheets lend storage to the engine. */
struct bind_style_context {
	struct css_engine *engine;
	struct wb_vector sheets;
	uint32_t generation;
};

static void style_context_destroy(struct bind_style_context *context);
static int style_context_gather(struct bind_style_context *context, struct bind_window *window);
static int style_context_add(struct bind_style_context *context, struct dom_element *element);
static int style_context_media(struct bind_style_context *context, struct dom_element *element, const struct css_media **media);

/*
 * Supplies active child styling to the Page host's synchronous layout observation.
 */
int
bind_window_child_styles(
	struct bind_window *window,
	struct css_engine **engine,
	int *width,
	int *height)
{
	int status;

	/* No borrowed engine may outlive a retired or unrelated binding context. */
	*engine = NULL;
	*width = 0;
	*height = 0;

	/* A retired Window supplies no child cascade or viewport. */
	if (window == NULL || window->detached)
		return 0;

	/* Refreshes the cascade before lending it to the page's layout observer. */
	status = bind_style_context_engine(window, engine);
	if (status != 0)
		return status;

	/* Parent geometry callbacks can retire the child while its viewport is refreshed. */
	if (window->detached || *engine == NULL) {
		*engine = NULL;
		return 0;
	}

	/* Preserve exact zero dimensions instead of substituting the parent viewport. */
	*width = window->viewport_width;
	*height = window->viewport_height;

	/* Succeeded: this synchronous caller can lay out the actual child Document. */
	return 0;
}

/*
 * Obtains the current child Document's cascade and selector engine.
 *
 * A replacement is published only after all connected inline sheets are ready.
 */
int
bind_style_context_engine(
	struct bind_window *window,
	struct css_engine **engine)
{
	struct bind_style_context *context;
	struct bind_style_context *old;
	int error;

	/* Primary hosts and retired child Documents have no fallback cascade. */
	*engine = NULL;

	/* Only an active managed child owns this fallback cache. */
	if (!window->owned ||
	    window->context_depth == 0 ||
	    window->detached)
		return 0;

	/* Parent layout changes can resize this child without changing its own Document generation. */
	error = bind_frame_viewport(window);
	if (error != 0)
		return error;

	/* The parent's geometry callback may have retired this managed child. */
	if (window->detached)
		return 0;

	/* Reuses only a cache built from the current Document generation. */
	context = window->style_context;
	if (context == NULL || context->generation != window->document->generation) {
		/* Allocates the replacement without disturbing the current C cache. */
		context = calloc(1, sizeof(*context));
		if (context == NULL)
			return ENOMEM;

		/* The vector owns parsed sheets after their successful insertion. */
		wb_vector_init(&context->sheets, sizeof(struct css_sheet *));
		context->generation = window->document->generation;

		/* Creates the existing engine with its ordinary UA defaults. */
		error = css_engine_create(&context->engine, window->realm->heap);
		if (error != 0) {
			style_context_destroy(context);
			return error;
		}

		/* Collects this Document's author sheets in connected tree order. */
		error = style_context_gather(context, window);
		if (error != 0) {
			style_context_destroy(context);
			return error;
		}

		/* Retires the prior C cache after the complete replacement is published. */
		old = window->style_context;
		window->style_context = context;
		style_context_destroy(old);
	}

	/* Viewport changes invalidate the engine's own computed-style cache. */
	css_engine_set_viewport(context->engine, window->viewport_width, window->viewport_height);
	*engine = context->engine;

	/* Succeeded: the caller can query the child's current cascade. */
	return 0;
}

/*
 * Computes a connected child's style with its actual ancestor inheritance.
 */
int
bind_style_context_compute(
	struct bind_window *window,
	struct dom_element *element,
	struct css_style *style)
{
	struct css_engine *engine;
	struct wb_vector ancestors;
	struct dom_node *walk;
	struct dom_element *ancestor;
	struct css_style *inherited;
	struct css_style *parent;
	size_t index;
	int error;

	/* A retired or differently owned element has no resolved declaration. */
	if (element->node.document != window->document || window->detached)
		return ENOENT;

	/* Records the ancestor elements without allocating styles on the C stack. */
	wb_vector_init(&ancestors, sizeof(struct dom_element *));
	walk = &element->node;
	while (walk != NULL && walk->type != DOM_DOCUMENT) {
		/* Includes only element ancestors in the cascade's inheritance chain. */
		if (walk->type == DOM_ELEMENT) {
			ancestor = (struct dom_element *)walk;
			error = wb_vector_push(&ancestors, &ancestor);
			if (error != 0) {
				wb_vector_release(&ancestors);
				return error;
			}
		}

		/* Continues toward the connected Document root. */
		walk = dom_flat_parent(walk);
	}

	/* Disconnected subtrees have no CSSOM resolved values. */
	if (walk != &window->document->node) {
		wb_vector_release(&ancestors);
		return ENOENT;
	}

	/* Readies the child engine only after establishing connected ownership. */
	error = bind_style_context_engine(window, &engine);
	if (error != 0) {
		wb_vector_release(&ancestors);
		return error;
	}

	/* Empty-host primary windows still have no fallback engine. */
	if (engine == NULL) {
		wb_vector_release(&ancestors);
		return ENOENT;
	}

	/* Keeps the previous ancestor's computed inheritance in checked C storage. */
	inherited = malloc(sizeof(*inherited));
	if (inherited == NULL) {
		wb_vector_release(&ancestors);
		return ENOMEM;
	}

	/* Computes from the outermost ancestor down to the requested element. */
	parent = NULL;
	error = 0;
	for (index = ancestors.count; index != 0; index--) {
		ancestor = *(struct dom_element **)wb_vector_at(&ancestors, index - 1U);
		error = css_engine_compute(engine, ancestor, parent, style);
		if (error != 0)
			break;

		/* Passes this ancestor's resolved inheritance to the next child. */
		*inherited = *style;
		parent = inherited;
	}

	/* Releases scratch storage without releasing the engine-backed style pointers. */
	free(inherited);
	wb_vector_release(&ancestors);

	/* Propagates a failed ancestor computation after releasing its scratch storage. */
	if (error != 0)
		return error;

	/* Succeeded: style pointers remain backed by the Window's current engine. */
	return 0;
}

/*
 * Releases the child cascade without inspecting possibly finalized DOM cells.
 */
void
bind_style_context_release(
	struct bind_window *window)
{
	/* No permanent VM roots are registered by the C context. */
	style_context_destroy(window->style_context);
	window->style_context = NULL;

	/* Succeeded: the Window no longer owns CSS engine storage. */
	return;
}

/* Releases engine caches before the parsed sheets whose storage they borrow. */
static void
style_context_destroy(
	struct bind_style_context *context)
{
	struct css_sheet *sheet;
	size_t index;

	/* Partial construction may not have allocated a context yet. */
	if (context == NULL)
		return;

	/* Destroys borrowed references before their sheet arenas disappear. */
	css_engine_destroy(context->engine);

	/* Parsed sheet destruction reads C storage, never DOM cells. */
	for (index = 0; index < context->sheets.count; index++) {
		sheet = *(struct css_sheet **)wb_vector_at(&context->sheets, index);
		css_sheet_destroy(sheet);
	}

	/* Discards the owning vector and the cache itself. */
	wb_vector_release(&context->sheets);
	free(context);

	/* Succeeded: all resources owned by this C cache have been released. */
	return;
}

/* Collects inline author sheets in tree order without recursive C traversal. */
static int
style_context_gather(
	struct bind_style_context *context,
	struct bind_window *window)
{
	struct dom_node *root;
	struct dom_node *walk;
	struct dom_element *element;
	int error;

	/* Walks only the active Document tree, excluding detached and template trees. */
	root = &window->document->node;
	walk = bind_following(root, root);
	while (walk != NULL) {
		/* Only HTML style elements contribute inline author sheets. */
		if (walk->type == DOM_ELEMENT) {
			element = (struct dom_element *)walk;
			if (element->ns == DOM_NS_HTML && element->tag == DOM_TAG_STYLE) {
				error = style_context_add(context, element);
				if (error != 0)
					return error;
			}
		}

		/* Advances to the next connected node in document order. */
		walk = bind_following(walk, root);
	}

	/* Succeeded: every inline author sheet is ready. */
	return 0;
}

/* Parses one style element and lends its sheet to the context's engine. */
static int
style_context_add(
	struct bind_style_context *context,
	struct dom_element *element)
{
	struct wb_units units;
	struct css_sheet *sheet;
	const struct css_media *media;
	int error;

	/* Primary and managed-child rendering share the actual native inline source association. */
	wb_units_init(&units);
	error = bind_style_sheet_source(element, &units);
	if (error != 0) {
		wb_units_release(&units);
		return error;
	}

	/* A non-memory parse failure contributes an empty sheet, as on primary pages. */
	sheet = NULL;
	error = css_sheet_create(&sheet, element->node.document->heap, units.data, units.length);
	if (error != 0) {
		wb_units_release(&units);
		css_sheet_destroy(sheet);

		/* Allocation failure must prevent publication of an incomplete cascade. */
		if (error == ENOMEM)
			return error;

		/* A syntactically unusable sheet contributes no author rules. */
		return 0;
	}

	/* The parsed sheet owns its source data independently of this temporary buffer. */
	wb_units_release(&units);

	/* Transfers ownership before the engine starts borrowing the parsed sheet. */
	error = wb_vector_push(&context->sheets, &sheet);
	if (error != 0) {
		css_sheet_destroy(sheet);
		return error;
	}

	/* Parses the style element's media attribute into engine-owned storage. */
	error = style_context_media(context, element, &media);
	if (error != 0)
		return error;

	/* Adds the author sheet after all preceding sheets in tree order. */
	error = css_engine_add_parsed(context->engine, sheet, &media, 1U);
	if (error != 0)
		return error;

	/* Succeeded: this sheet participates under its media list. */
	return 0;
}

/* Parses a style element's media attribute, with absence meaning all media. */
static int
style_context_media(
	struct bind_style_context *context,
	struct dom_element *element,
	const struct css_media **media)
{
	struct vm_string *attribute;
	struct wb_units units;
	size_t index;
	uint32_t point;
	int error;

	/* A missing media attribute imposes no condition on the sheet. */
	*media = NULL;

	/* An absent attribute selects the unconditional author-sheet media list. */
	attribute = dom_attribute_ascii(element, "media");
	if (attribute == NULL)
		return 0;

	/* Reads the VM string without assuming its encoding or contiguity. */
	wb_units_init(&units);
	for (index = 0; index < attribute->length; index++) {
		point = vm_string_at(attribute, index);
		error = wb_units_append_code_point(&units, point);
		if (error != 0) {
			wb_units_release(&units);
			return error;
		}
	}

	/* The engine keeps the parsed list in its own arena. */
	error = css_engine_parse_media(context->engine, units.data, units.length, media);
	if (error != 0) {
		wb_units_release(&units);
		return error;
	}

	/* The arena-backed media list no longer needs the temporary attribute buffer. */
	wb_units_release(&units);

	/* Succeeded: the list is available until the context is released. */
	return 0;
}
