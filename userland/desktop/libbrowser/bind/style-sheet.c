/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Owns traced inline CSS sources independently of primary or managed-child C engines.
 */

#include "bind/internal.h"
#include "css/css.h"

#include <errno.h>

static int sheet_original(struct dom_element *element, struct wb_units *units);
static void sheet_trace(struct vm_heap *heap, struct vm_cell *cell);
static void sheet_finalize(struct vm_heap *heap, struct vm_cell *cell);

/*
 * Obtains the actual current source state of a connected HTML style element.
 */
int
bind_style_sheet_get(
	struct dom_element *element,
	struct bind_style_sheet **sheet)
{
	/* This descriptor owns traced state storage without registering a permanent root. */
	static const struct vm_cell_type sheet_type = { "inline-style-source", sheet_trace, sheet_finalize };
	struct vm_heap *heap;
	struct vm_cell *root;
	struct bind_style_sheet *made;
	struct css_rule_model *model;
	struct wb_units units;
	int same;
	int connected;
	int error;

	/* Only genuine connected HTML style elements can have a current inline source. */
	*sheet = NULL;
	if (element == NULL || element->ns != DOM_NS_HTML)
		return 0;
	same = vm_string_equal_ascii(element->local_name, "style");
	if (!same)
		return 0;
	connected = dom_is_connected(&element->node) || element->constructed_sheet;
	if (!connected)
		return 0;

	/* DOM-driven child and connection changes already retired any previous current identity. */
	if (element->style_sheet != NULL) {
		*sheet = (struct bind_style_sheet *)element->style_sheet;
		return 0;
	}

	/* The actual owner graph survives parser atoms and state allocation even without native stack roots. */
	heap = element->node.document->heap;
	root = &element->node.cell;
	error = vm_heap_add_root(heap, &root);
	if (error != 0)
		return error;

	/* Copy the direct DOM text while its native owner remains temporarily rooted. */
	wb_units_init(&units);
	error = sheet_original(element, &units);
	if (error != 0) {
		wb_units_release(&units);
		vm_heap_remove_root(heap, &root);
		return error;
	}

	/* The pure CSS model owns copied source entries independently of these original DOM units. */
	model = NULL;
	error = css_rule_model_create(&model, heap, units.data, units.length);
	if (error != 0) {
		wb_units_release(&units);
		vm_heap_remove_root(heap, &root);
		return error;
	}

	/* Allocate traced source storage only after all fallible parser work succeeded. */
	made = vm_heap_alloc(heap, &sheet_type, sizeof(*made));
	if (made == NULL) {
		css_rule_model_destroy(model);
		wb_units_release(&units);
		vm_heap_remove_root(heap, &root);
		return ENOMEM;
	}

	/* Complete native ownership before publishing the current association. */
	made->owner = element;
	made->model = model;
	made->original = units;

	/* Unchanged source retains the original DOM text until a native CSSOM edit is observed. */
	made->changed = 0;

	/* Wrapper identities are created lazily and are traced from this retained source state. */
	made->wrapper = NULL;
	made->rules = NULL;

	/* Publish the fully initialized source before dropping its temporary owner hold. */
	element->style_sheet = &made->cell;
	*sheet = made;
	vm_heap_remove_root(heap, &root);

	/* Succeeded: native owner tracing retains the current immutable source model. */
	return 0;
}

/*
 * Appends unchanged original DOM source or genuinely modified native model source for rendering.
 */
int
bind_style_sheet_source(
	struct dom_element *element,
	struct wb_units *units)
{
	struct bind_style_sheet *sheet;
	int error;

	/* Rendering never borrows native model pointers beyond this synchronous copied source query. */
	error = bind_style_sheet_get(element, &sheet);
	if (error != 0)
		return error;

	/* A disabled sheet gives the page nothing. */
	if (sheet != NULL && sheet->disabled)
		return 0;

	/* Existing non-associated source handling keeps its original rendering semantics. */
	if (sheet == NULL) {
		error = sheet_original(element, units);
	} else if (sheet->changed) {
		error = css_rule_model_text(sheet->model, units);
	} else {
		error = wb_units_append(units, sheet->original.data, sheet->original.length);
	}

	/* Any source-copy failure prevents a partially updated style engine. */
	if (error != 0)
		return error;

	/* Succeeded: the existing stylesheet parser receives actual current source units. */
	return 0;
}

/*
 * Invalidates current rendering after a genuine native source-model edit without DOM mutation.
 */
void
bind_style_sheet_changed(
	struct bind_style_sheet *sheet)
{
	struct dom_element *owner;
	int connected;

	/* Source queries now serialize the edited native model instead of the original DOM text. */
	sheet->changed = 1;

	/* A saved old model can change independently without changing the owner's current association. */
	owner = sheet->owner;
	if (owner->style_sheet != &sheet->cell)
		return;

	/* Detached owners must not invalidate the rendering generation of a current Document. */
	connected = dom_is_connected(&owner->node) || owner->constructed_sheet;
	if (!connected)
		return;

	/* Native cascade and layout caches observe this generation without fabricated mutation records. */
	owner->node.document->generation++;

	/* Succeeded: only an actual current sheet model invalidated the owner's rendering. */
	return;
}

/* Copies only direct Text children, excluding comments, descendants and template contents. */
static int
sheet_original(
	struct dom_element *element,
	struct wb_units *units)
{
	struct dom_node *child;
	struct dom_character_data *text;
	int error;

	/* Direct Text content is the same original stylesheet source the existing Page parser consumed. */
	for (child = element->node.first_child;
	     child != NULL;
	     child = child->next) {
		/* Comments and descendants are excluded from the direct stylesheet source. */
		if (child->type != DOM_TEXT && child->type != DOM_CDATA_SECTION)
			continue;

		/* Append this direct text node without lending its owned units to the renderer. */
		text = (struct dom_character_data *)child;
		error = wb_units_append(units, text->data.data, text->data.length);
		if (error != 0)
			return error;
	}

	/* Succeeded: the caller owns a copy of the actual original Text content. */
	return 0;
}

/* Marks only genuine native owner edges; the CSS model owns no heap-cell references. */
static void
sheet_trace(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct bind_style_sheet *sheet;

	/* The actual owner keeps its Document, binding realm and native identity graph reachable. */
	sheet = (struct bind_style_sheet *)cell;
	vm_heap_mark(heap, &sheet->owner->node.cell);

	/* Public wrapper identity belongs to this actual source even after DOM retires it. */
	if (sheet->wrapper != NULL)
		vm_heap_mark(heap, &sheet->wrapper->cell);

	/* A lazily created rule collection retains its identity through the same native source. */
	if (sheet->rules != NULL)
		vm_heap_mark(heap, &sheet->rules->cell);

	/* Succeeded: every native owner edge of the retained source state was traced. */
	return;
}

/* Frees C source storage without reading possibly finalized DOM or Window cells. */
static void
sheet_finalize(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct bind_style_sheet *sheet;

	UNUSED_PARAMETER(heap);

	/* Finalizer order is irrelevant because only independently owned C buffers are destroyed. */
	sheet = (struct bind_style_sheet *)cell;
	css_rule_model_destroy(sheet->model);
	wb_units_release(&sheet->original);

	/* Succeeded: no native source model or original Text buffer remains allocated. */
	return;
}
