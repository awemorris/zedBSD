/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Native inline sheet wrappers share actual source identity and live Document membership. */

#include "bind/internal.h"
#include "css/css.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>

/* Each private state is one owner query, inline sheet or saved source rule-count view. */
enum cssom_kind {
	CSSOM_SHEETS,
	CSSOM_SHEET,
	CSSOM_RULES
};

/* The wrapper traces this state, which retains its actual Document and optional native source. */
struct cssom_state {
	struct vm_cell cell;
	enum cssom_kind kind;
	struct dom_document *document;
	struct bind_style_sheet *source;
};

/* Sheet access belongs to actual HTML style elements rather than arbitrary HTMLElement expandos. */
static const struct bind_attribute cssom_style_attributes[] = {
	{ "sheet", bind_cssom_style_sheet, NULL },
	{ NULL, NULL, NULL }
};

/* Inline sheets currently expose actual owner identity and their absent external URL. */
static const struct bind_attribute cssom_sheet_attributes[] = {
	{ "ownerNode", bind_cssom_owner, NULL },
	{ "href", bind_cssom_href, NULL },
	{ NULL, NULL, NULL }
};

/* The source model owns its SameObject rule-count view independently of later DOM source changes. */
static const struct bind_attribute cssom_css_attributes[] = {
	{ "cssRules", bind_cssom_rules, NULL },
	/* The legacy name of the same list. */
	{ "rules", bind_cssom_rules, NULL },
	{ NULL, NULL, NULL }
};

/* Genuine ordinary rule edits mutate native source storage without touching DOM Text. */
static const struct bind_operation cssom_css_operations[] = {
	{ "insertRule", 1, bind_cssom_insert },
	{ "deleteRule", 1, bind_cssom_delete },
	{ NULL, 0, NULL }
};

/* Supported sheet indices remain virtual while length follows the current actual owner tree. */
static const struct bind_attribute cssom_sheets_attributes[] = {
	{ "length", bind_cssom_sheets_length, NULL },
	{ NULL, NULL, NULL }
};

/* Only the native indexed query supplies a sheet through item(). */
static const struct bind_operation cssom_sheets_operations[] = {
	{ "item", 1, bind_cssom_sheets_item },
	{ NULL, 0, NULL }
};

/* This bounded rule-list core reads authoritative model count; rule object APIs follow separately. */
static const struct bind_attribute cssom_rules_attributes[] = {
	{ "length", bind_cssom_rules_length, NULL },
	{ NULL, NULL, NULL }
};

/* HTML style construction uses the ordinary DOM element factory. */
const struct bind_interface bind_html_style_element_interface = {
	"HTMLStyleElement", BIND_HTML_ELEMENT, 0, NULL, cssom_style_attributes, NULL, NULL
};

/* Public sheet interfaces cannot be constructed through the current binding. */
const struct bind_interface bind_style_sheet_interface = {
	"StyleSheet", BIND_NO_PARENT, 0, NULL, cssom_sheet_attributes, NULL, NULL
};

/* The current native inline CSS sheet inherits ordinary StyleSheet metadata. */
const struct bind_interface bind_css_style_sheet_interface = {
	"CSSStyleSheet", BIND_STYLE_SHEET, 0, NULL, cssom_css_attributes, cssom_css_operations, NULL
};

/* Actual Document membership creates this live native indexed sheet query. */
const struct bind_interface bind_style_sheet_list_interface = {
	"StyleSheetList", BIND_NO_PARENT, 0, NULL, cssom_sheets_attributes, cssom_sheets_operations, NULL
};

/* Genuine source ownership creates the independently retained live count core. */
const struct bind_interface bind_css_rule_list_interface = {
	"CSSRuleList", BIND_NO_PARENT, 0, NULL, cssom_rules_attributes, NULL, NULL
};

static int cssom_insert_text(struct cssom_state *state, const uint16_t *units, size_t length, uint32_t index);
static int cssom_mutation_error(struct vm_realm *realm, int status);
static void cssom_trace(struct vm_heap *heap, struct vm_cell *cell);
static const struct vm_cell_type *cssom_type(void);
static const struct vm_native_operations *cssom_native(void);
static int cssom_make(struct vm_realm *realm, struct dom_document *document, struct bind_style_sheet *source, enum cssom_kind kind, struct vm_object **out);
static int cssom_this(struct vm_realm *realm, vm_value receiver, enum cssom_kind kind, struct cssom_state **out);
static int cssom_wrap(struct vm_realm *realm, struct dom_element *element, vm_value *result);
static struct dom_node *cssom_next(struct dom_document *document, struct dom_node *node);
static int cssom_member(struct dom_node *node);
static struct dom_element *cssom_index(struct dom_document *document, uint32_t index);
static int cssom_count(struct dom_document *document, uint32_t *count);
static int cssom_key(vm_value key, uint32_t *index);
static int cssom_get_own(struct vm_object *object, vm_value key, struct vm_property *property);
static int cssom_own_keys(struct vm_heap *heap, struct vm_object *object, struct wb_vector *keys);
static int cssom_define(struct vm_realm *realm, struct vm_object *object, vm_value key, const struct vm_descriptor *descriptor, int *handled, int *done);
static int cssom_delete(struct vm_heap *heap, struct vm_object *object, vm_value key, int *handled, int *deleted);
static int cssom_prevent_extensions(struct vm_object *object, int *allowed);

/*
 * Returns the actual Document's traced SameObject live inline sheet query.
 */
int
bind_cssom_document_sheets(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_node *node;
	struct dom_document *document;
	struct vm_object *wrapper;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Only the actual native Document brand can own a sheet query. */
	status = bind_this_node(realm, receiver, &node);
	if (status != 0)
		return status;
	if (node->type != DOM_DOCUMENT) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Publish a complete wrapper only after checked native state construction succeeds. */
	document = (struct dom_document *)node;
	if (document->style_sheets == NULL) {
		status = cssom_make(realm, document, NULL, CSSOM_SHEETS, &wrapper);
		if (status != 0)
			return status;
		document->style_sheets = wrapper;
	}

	/* Succeeded: the saved query observes the actual current Document tree. */
	*result = vm_value_cell(document->style_sheets);
	return 0;
}

/*
 * Returns the current actual inline sheet or null for an unconnected style element.
 */
int
bind_cssom_style_sheet(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_node *node;
	int actual;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The HTML namespace and exact local name remain authoritative after prototype changes. */
	status = bind_this_node(realm, receiver, &node);
	if (status != 0)
		return status;
	actual = cssom_member(node);
	if (!actual) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Succeeded: source association itself determines whether a current sheet exists. */
	status = cssom_wrap(realm, (struct dom_element *)node, result);
	return status;
}

/*
 * Reports the actual current owner, or null for a source retired by DOM lifecycle.
 */
int
bind_cssom_owner(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct cssom_state *state;
	struct dom_element *owner;
	int connected;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* A private native sheet state establishes the brand independently of prototypes. */
	status = cssom_this(realm, receiver, CSSOM_SHEET, &state);
	if (status != 0)
		return status;
	*result = VM_VALUE_NULL;
	owner = state->source->owner;
	if (owner->style_sheet != &state->source->cell)
		return 0;
	connected = dom_is_inclusive_ancestor(&owner->node.document->node, &owner->node);
	if (!connected)
		return 0;

	/* Succeeded: native wrapping preserves actual owner identity and its relevant realm. */
	status = bind_wrap(NULL, &owner->node, result);
	return status;
}

/*
 * Reports null for the genuine inline sheet's absent external location.
 */
int
bind_cssom_href(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct cssom_state *state;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Inline origin metadata still requires an actual native sheet brand. */
	status = cssom_this(realm, receiver, CSSOM_SHEET, &state);
	if (status != 0)
		return status;

	/* Succeeded: no external URL was invented for an actual inline source. */
	*result = VM_VALUE_NULL;
	return 0;
}

/*
 * Returns this genuine source's SameObject native rule-count view.
 */
int
bind_cssom_rules(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct cssom_state *state;
	struct vm_object *wrapper;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Saved old sheets keep their own model instead of resolving current DOM source. */
	status = cssom_this(realm, receiver, CSSOM_SHEET, &state);
	if (status != 0)
		return status;
	if (state->source->rules == NULL) {
		status = cssom_make(realm, state->document, state->source, CSSOM_RULES, &wrapper);
		if (status != 0)
			return status;
		state->source->rules = wrapper;
	}

	/* Succeeded: current model edits remain visible through this saved actual source view. */
	*result = vm_value_cell(state->source->rules);
	return 0;
}

/*
 * Counts current eligible inline source owners in the actual Document tree.
 */
int
bind_cssom_sheets_length(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct cssom_state *state;
	uint32_t length;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Other native CSSOM wrappers do not share this collection brand. */
	status = cssom_this(realm, receiver, CSSOM_SHEETS, &state);
	if (status != 0)
		return status;
	status = cssom_count(state->document, &length);
	if (status != 0)
		return status;

	/* Succeeded: length is an unsigned count without signed index aliasing. */
	*result = vm_value_double((double)length);
	return 0;
}

/*
 * Selects a current native inline sheet after ordinary WebIDL index conversion.
 */
int
bind_cssom_sheets_item(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct cssom_state *state;
	struct dom_element *element;
	struct vm_cell *root;
	vm_value value;
	uint32_t index;
	int status;

	/* Native identity is validated before potentially reentrant index conversion. */
	status = cssom_this(realm, receiver, CSSOM_SHEETS, &state);
	if (status != 0)
		return status;
	root = &state->cell;
	status = vm_heap_add_root(state->document->heap, &root);
	if (status != 0)
		return status;
	value = VM_VALUE_UNDEFINED;
	if (count != 0)
		value = args[0];
	status = vm_to_uint32(realm, value, &index);
	if (status != 0) {
		vm_heap_remove_root(state->document->heap, &root);
		return status;
	}

	/* Current native membership is read after conversion may have changed actual DOM links. */
	element = cssom_index(state->document, index);
	*result = VM_VALUE_NULL;
	if (element != NULL)
		status = cssom_wrap(realm, element, result);
	vm_heap_remove_root(state->document->heap, &root);

	/* Succeeded or failed: no temporary collection owner remains rooted. */
	return status;
}

/*
 * Reads current top-level rule count from the genuine saved native source model.
 */
int
bind_cssom_rules_length(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct cssom_state *state;
	size_t length;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Sheet and Document query states cannot masquerade as native rule lists. */
	status = cssom_this(realm, receiver, CSSOM_RULES, &state);
	if (status != 0)
		return status;
	length = css_rule_model_count(state->source->model);

	/* Succeeded: one preserved top-level group remains one rule regardless of rendering matches. */
	*result = vm_value_double((double)length);
	return 0;
}

/*
 * Inserts one ordinary qualified rule into a genuine native inline sheet model.
 */
int
bind_cssom_insert(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct cssom_state *state;
	struct vm_heap *heap;
	struct vm_cell *roots[3];
	struct vm_string *string;
	struct wb_units units;
	uint32_t index;
	unsigned registered;
	unsigned slot;
	size_t offset;
	int cell;
	int status;

	/* Genuine private sheet identity is established before any user conversion runs. */
	status = cssom_this(realm, receiver, CSSOM_SHEET, &state);
	if (status != 0)
		return status;
	if (count == 0) {
		status = vm_throw_type_error(realm, "insertRule requires a rule argument.");
		return status;
	}

	/* Nullable argument roots protect direct native calls as well as ordinary interpreter invocations. */
	heap = state->document->heap;
	roots[0] = &state->source->cell;
	roots[1] = NULL;
	roots[2] = NULL;
	cell = vm_value_is_cell(args[0]);
	if (cell)
		roots[1] = vm_value_as_cell(args[0]);
	if (count > 1U) {
		cell = vm_value_is_cell(args[1]);
		if (cell)
			roots[2] = vm_value_as_cell(args[1]);
	}

	/* Register every slot before conversions or parser allocations can collect the saved source graph. */
	registered = 0;
	for (slot = 0; slot < 3U; slot++) {
		status = vm_heap_add_root(heap, &roots[slot]);
		if (status != 0)
			break;
		registered++;
	}

	/* Independent copied source storage is initialized even on partial root-registration failure. */
	wb_units_init(&units);
	index = 0;
	if (status == 0) {
		do {
			/* WebIDL converts the rule before the optional index; retain the actual returned string. */
			status = vm_to_string(realm, args[0], &string);
			if (status != 0)
				break;
			roots[1] = &string->cell;
			if (count > 1U) {
				status = vm_to_uint32(realm, args[1], &index);
				if (status != 0)
					break;
			}

			/* Copy exact UTF16 independently of whether the native string stores Latin1 or wide units. */
			status = wb_units_reserve(&units, string->length);
			if (status != 0)
				break;
			for (offset = 0; offset < string->length; offset++)
				units.data[offset] = vm_string_at(string, offset);
			units.length = string->length;

			/* Validate before committing to this actual saved source, even if conversion retired its owner. */
			status = cssom_insert_text(state, units.data, units.length, index);
			if (status != 0)
				break;
			bind_style_sheet_changed(state->source);
		} while (0);
	}

	/* Error construction still retains the actual source and its relevant managed realm. */
	if (status != 0)
		status = cssom_mutation_error(realm, status);

	/* No copied source or temporary owner, input or converted string root survives this invocation. */
	wb_units_release(&units);
	for (slot = 0; slot < registered; slot++)
		vm_heap_remove_root(heap, &roots[slot]);
	if (status != 0)
		return status;

	/* Succeeded: the actual inserted index is returned without changing DOM Text. */
	*result = vm_value_double((double)index);
	return 0;
}

/*
 * Deletes one current rule from the genuine saved inline sheet model.
 */
int
bind_cssom_delete(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct cssom_state *state;
	struct vm_heap *heap;
	struct vm_cell *roots[2];
	uint32_t index;
	unsigned slot;
	unsigned registered;
	int cell;
	int status;

	/* Required argument handling follows the actual native sheet brand check. */
	status = cssom_this(realm, receiver, CSSOM_SHEET, &state);
	if (status != 0)
		return status;
	if (count == 0) {
		status = vm_throw_type_error(realm, "deleteRule requires an index argument.");
		return status;
	}

	/* The saved source and index object remain reachable during reentrant index conversion. */
	heap = state->document->heap;
	roots[0] = &state->source->cell;
	roots[1] = NULL;
	cell = vm_value_is_cell(args[0]);
	if (cell)
		roots[1] = vm_value_as_cell(args[0]);
	registered = 0;
	for (slot = 0; slot < 2U; slot++) {
		status = vm_heap_add_root(heap, &roots[slot]);
		if (status != 0)
			break;
		registered++;
	}

	/* Conversion can change the current DOM source; the actual receiver still owns its original model. */
	if (status == 0)
		status = vm_to_uint32(realm, args[0], &index);
	if (status == 0)
		status = css_rule_model_delete(state->source->model, index);
	if (status == 0)
		bind_style_sheet_changed(state->source);

	/* Keep the actual owner graph alive while constructing any native DOM exception. */
	if (status != 0)
		status = cssom_mutation_error(realm, status);
	for (slot = 0; slot < registered; slot++)
		vm_heap_remove_root(heap, &roots[slot]);
	if (status != 0)
		return status;

	/* Succeeded: saved and current rule counts observe the actual native deletion. */
	*result = VM_VALUE_UNDEFINED;
	return 0;
}

/*
 * Reports one rule of a sheet as the text "id type source" for the script-side rule objects.
 */
int
bind_cssom_rule_info(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct cssom_state *state;
	struct vm_string *string;
	struct wb_units units;
	const uint16_t *source;
	size_t length;
	uint32_t index;
	uint32_t id;
	unsigned closing;
	char head[48];
	int type;
	int present;
	int status;
	int used;
	int offset;

	*result = VM_VALUE_UNDEFINED;
	status = cssom_this(realm, receiver, CSSOM_SHEET, &state);
	if (status != 0)
		return status;
	if (count == 0)
		return 0;
	status = vm_to_uint32(realm, args[0], &index);
	if (status != 0)
		return status;
	if (index >= css_rule_model_count(state->source->model))
		return 0;
	id = css_rule_model_id(state->source->model, index);
	status = css_rule_model_source(state->source->model, id, &source, &length, &type, &present);
	if (status != 0)
		return status;

	/* The head, the source, and the closings the source omitted at its end. */
	used = snprintf(head, sizeof(head), "%u %d ", (unsigned)id, type);
	wb_units_init(&units);
	status = 0;
	for (offset = 0; offset < used && status == 0; offset++)
		status = wb_units_append_code_point(&units, (uint32_t)head[offset]);
	if (status == 0)
		status = wb_units_append(&units, source, length);
	closing = css_rule_model_closing(state->source->model, id);
	while (status == 0 && closing > 0) {
		status = wb_units_append_code_point(&units, '}');
		closing--;
	}
	if (status == 0) {
		string = vm_string_from_units(state->document->heap, units.data, units.length);
		if (string == NULL)
			status = ENOMEM;
		else
			*result = vm_value_cell(&string->cell);
	}
	wb_units_release(&units);
	return status;
}

/*
 * Replaces one rule of a sheet with new source text; the result is the rule's new identity or throws.
 */
int
bind_cssom_rule_replace(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct cssom_state *state;
	struct vm_string *string;
	struct wb_units units;
	uint32_t index;
	size_t offset;
	int status;

	status = cssom_this(realm, receiver, CSSOM_SHEET, &state);
	if (status != 0)
		return status;
	if (count < 2U)
		return vm_throw_type_error(realm, "ruleReplace requires an index and a rule.");
	status = vm_to_uint32(realm, args[0], &index);
	if (status != 0)
		return status;
	status = vm_to_string(realm, args[1], &string);
	if (status != 0)
		return status;
	wb_units_init(&units);
	status = wb_units_reserve(&units, string->length);
	if (status == 0) {
		for (offset = 0; offset < string->length; offset++)
			units.data[offset] = vm_string_at(string, offset);
		units.length = string->length;
		status = css_rule_model_replace(state->source->model, units.data, units.length, index);
		if (status == 0)
			bind_style_sheet_changed(state->source);
	}
	wb_units_release(&units);
	if (status != 0)
		return cssom_mutation_error(realm, status);
	*result = vm_value_double((double)css_rule_model_id(state->source->model, index));
	return 0;
}

/* Marks actual owner graphs without relying on live Window pointers or conservative stack discovery. */
static void
cssom_trace(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct cssom_state *state;

	/* The actual Document retains its relevant realm or private prototype snapshot. */
	state = (struct cssom_state *)cell;
	vm_heap_mark(heap, &state->document->node.cell);
	if (state->source != NULL)
		vm_heap_mark(heap, &state->source->cell);

	/* Succeeded: every native query or source owner remains reachable. */
	return;
}

/* Supplies one private cell identity while keeping callback declarations before descriptor use. */
static const struct vm_cell_type *
cssom_type(
	void)
{
	/* The descriptor lives for the process and gives every native CSSOM state the same private brand. */
	static const struct vm_cell_type type = { "inline-cssom", cssom_trace, NULL };

	/* Succeeded: callers compare actual private type identity. */
	return &type;
}

/* Supplies genuine legacy index hooks independently of a wrapper's public prototype. */
static const struct vm_native_operations *
cssom_native(
	void)
{
	/* Only Document sheet lists use indices; sheet and rule-count states fall through ordinary keys. */
	static const struct vm_native_operations operations = {
		cssom_get_own, cssom_own_keys, cssom_define, cssom_delete, cssom_prevent_extensions
	};

	/* Succeeded: callbacks share a native brand without public expando metadata. */
	return &operations;
}

/* Constructs a complete wrapper while actual owner and private state roots span every allocation. */
static int
cssom_make(
	struct vm_realm *realm,
	struct dom_document *document,
	struct bind_style_sheet *source,
	enum cssom_kind kind,
	struct vm_object **out)
{
	struct vm_heap *heap;
	struct bind_window *window;
	struct cssom_state *state;
	struct vm_object *prototype;
	struct vm_object *wrapper;
	struct vm_cell *owner_root;
	struct vm_cell *state_root;
	const struct vm_cell_type *type;
	vm_value snapshot;
	int index;
	int status;

	/* Root actual graph identity before prototype access, private state or object allocation. */
	*out = NULL;
	heap = document->heap;
	owner_root = &document->node.cell;
	if (source != NULL)
		owner_root = &source->cell;
	status = vm_heap_add_root(heap, &owner_root);
	if (status != 0)
		return status;
	index = BIND_STYLE_SHEET_LIST;
	if (kind == CSSOM_SHEET)
		index = BIND_CSS_STYLE_SHEET;
	if (kind == CSSOM_RULES)
		index = BIND_CSS_RULE_LIST;

	/* Resolve the actual owner's prototype rather than the borrowed getter's realm. */
	prototype = NULL;
	status = 0;
	if (document->binding_prototypes != NULL) {
		status = vm_object_get(document->binding_prototypes, vm_value_int32(index), &snapshot);
		if (status == 0) {
			if (snapshot == VM_VALUE_UNDEFINED) {
				status = EINVAL;
			} else {
				prototype = (struct vm_object *)vm_value_as_cell(snapshot);
			}
		}
	} else {
		window = document->view;
		if (window == NULL && realm != NULL)
			window = bind_window_of(realm);
		if (window == NULL) {
			status = EINVAL;
		} else {
			prototype = window->prototypes[index];
		}
	}

	/* Failed owner prototype resolution must unwind the protected graph. */
	if (status != 0) {
		vm_heap_remove_root(heap, &owner_root);
		return status;
	}

	/* A fully initialized private cell can safely be traced while its wrapper allocates. */
	type = cssom_type();
	state = vm_heap_alloc(heap, type, sizeof(*state));
	if (state == NULL) {
		vm_heap_remove_root(heap, &owner_root);
		return ENOMEM;
	}

	/* Publish no public edge until both owner fields and the independent state root exist. */
	state->kind = kind;
	state->document = document;
	state->source = source;
	state_root = &state->cell;
	status = vm_heap_add_root(heap, &state_root);
	if (status != 0) {
		vm_heap_remove_root(heap, &owner_root);
		return status;
	}

	/* The independent state root spans the final public object allocation. */
	wrapper = vm_object_create(heap, prototype);
	if (wrapper == NULL) {
		vm_heap_remove_root(heap, &state_root);
		vm_heap_remove_root(heap, &owner_root);
		return ENOMEM;
	}

	/* A real platform wrapper owns the private typed state and genuine native operations. */
	wrapper->kind = VM_KIND_PLATFORM;
	wrapper->internal = vm_value_cell(state);
	wrapper->native_operations = cssom_native();
	*out = wrapper;
	vm_heap_remove_root(heap, &state_root);
	vm_heap_remove_root(heap, &owner_root);

	/* Succeeded: the caller can publish this complete wrapper into its actual owner's cache. */
	return 0;
}

/* Rejects every prototype lookalike, foreign platform state and wrong CSSOM interface kind. */
static int
cssom_this(
	struct vm_realm *realm,
	vm_value receiver,
	enum cssom_kind kind,
	struct cssom_state **out)
{
	struct vm_object *object;
	struct vm_cell *cell;
	const struct vm_cell_type *type;
	const struct vm_native_operations *operations;
	int valid;
	int status;

	/* Ordinary objects cannot provide a native private cell by imitating a public prototype. */
	valid = vm_value_is_object(receiver);
	if (!valid) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* The genuine native operation table must match before inspecting internal storage. */
	object = (struct vm_object *)vm_value_as_cell(receiver);
	operations = cssom_native();
	if (object->kind != VM_KIND_PLATFORM || object->native_operations != operations) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Actual private cell identity and interface kind are independent brand checks. */
	cell = vm_value_as_cell(object->internal);
	type = cssom_type();
	if (cell->type != type) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Native sheet, Document query and rule-count brands remain distinct. */
	if (((struct cssom_state *)cell)->kind != kind) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Succeeded: only this genuine native interface state can reach owner metadata. */
	*out = (struct cssom_state *)cell;
	return 0;
}

/* Resolves actual source association before reusing or constructing a genuine public sheet. */
static int
cssom_wrap(
	struct vm_realm *realm,
	struct dom_element *element,
	vm_value *result)
{
	struct bind_style_sheet *source;
	struct vm_object *wrapper;
	int status;

	/* A disconnected style owns no current source or public sheet association. */
	*result = VM_VALUE_NULL;
	status = bind_style_sheet_get(element, &source);
	if (status != 0)
		return status;
	if (source == NULL)
		return 0;

	/* Each actual source owns one public sheet wrapper regardless of borrowed getter realm. */
	if (source->wrapper == NULL) {
		status = cssom_make(realm, element->node.document, source, CSSOM_SHEET, &wrapper);
		if (status != 0)
			return status;
		source->wrapper = wrapper;
	}

	/* Succeeded: source retirement cannot redirect this saved wrapper to a later model. */
	*result = vm_value_cell(source->wrapper);
	return 0;
}

/* Advances in actual Document preorder without entering template contents or iframe Documents. */
static struct dom_node *
cssom_next(
	struct dom_document *document,
	struct dom_node *node)
{
	struct dom_node *root;

	/* Only ordinary native tree links supply descendant membership. */
	root = &document->node;
	if (node == NULL)
		return root->first_child;
	if (node->first_child != NULL)
		return node->first_child;

	/* Ascend within this actual Document until a following sibling appears. */
	while (node != root) {
		if (node->next != NULL)
			return node->next;
		node = node->parent;
	}

	/* Exhausted: no following descendant exists in this actual Document. */
	return NULL;
}

/* Distinguishes exact HTML style owners from folded XML tags or foreign namespaces. */
static int
cssom_member(
	struct dom_node *node)
{
	struct dom_element *element;
	int same;

	/* Non-elements have no inline style source association. */
	if (node->type != DOM_ELEMENT)
		return 0;
	element = (struct dom_element *)node;
	if (element->ns != DOM_NS_HTML)
		return 0;
	same = vm_string_equal_ascii(element->local_name, "style");

	/* Succeeded: actual native local identity decides eligibility. */
	return same;
}

/* Selects an actual current eligible descendant without caching a stale DOM member list. */
static struct dom_element *
cssom_index(
	struct dom_document *document,
	uint32_t index)
{
	struct dom_node *node;
	int member;

	/* Every read traverses the actual current owner tree in sheet order. */
	node = cssom_next(document, NULL);
	while (node != NULL) {
		member = cssom_member(node);
		if (member) {
			if (index == 0)
				return (struct dom_element *)node;
			index--;
		}

		/* Continue through the actual current descendant links. */
		node = cssom_next(document, node);
	}

	/* Exhausted: the index has no current member. */
	return NULL;
}

/* Counts only currently eligible actual inline source owners, with checked unsigned bounds. */
static int
cssom_count(
	struct dom_document *document,
	uint32_t *count)
{
	struct dom_node *node;
	int member;

	/* No source parsing or model allocation is needed to inspect actual connected membership. */
	*count = 0;
	node = cssom_next(document, NULL);
	while (node != NULL) {
		member = cssom_member(node);
		if (member) {
			if (*count == UINT32_MAX)
				return EOVERFLOW;
			(*count)++;
		}

		/* Continue through the actual current descendant links. */
		node = cssom_next(document, node);
	}

	/* Succeeded: the current native membership fits the WebIDL unsigned length. */
	return 0;
}

/* Classifies all canonical uint32 indices even when the VM stores larger ones as strings. */
static int
cssom_key(
	vm_value key,
	uint32_t *index)
{
	struct vm_string *string;
	uint64_t number;
	size_t offset;
	uint16_t unit;
	int indexed;
	int is_string;

	/* Common indices use the VM's already normalized nonnegative int32 keys. */
	indexed = vm_value_is_array_index(key, index);
	if (indexed)
		return 1;
	is_string = vm_value_is_string(key);
	if (!is_string)
		return 0;
	string = (struct vm_string *)vm_value_as_cell(key);
	if (string->length == 0 || string->length > 10U)
		return 0;

	/* Leading zeroes are names, while the single zero is a valid canonical index. */
	unit = vm_string_at(string, 0);
	if (string->length > 1U && unit == '0')
		return 0;

	/* Ten decimal digits fit the wide accumulator before checking the array-index ceiling. */
	number = 0;
	for (offset = 0; offset < string->length; offset++) {
		unit = vm_string_at(string, offset);
		if (unit < '0' || unit > '9')
			return 0;
		number = number * 10U + (uint64_t)(unit - '0');
	}

	/* UINT32_MAX itself is a supported name candidate rather than an array index. */
	if (number >= UINT32_MAX)
		return 0;

	/* Succeeded: the complete unsigned index has no signed-int32 alias. */
	*index = (uint32_t)number;
	return 1;
}

/* Resolves current indexed sheet values while ordinary string and symbol properties remain separate. */
static int
cssom_get_own(
	struct vm_object *object,
	vm_value key,
	struct vm_property *property)
{
	struct cssom_state *state;
	struct dom_element *element;
	uint32_t index;
	int indexed;
	int status;

	/* Sheet and rule-count wrappers have no virtual own properties in this bounded core. */
	state = (struct cssom_state *)vm_value_as_cell(object->internal);
	if (state->kind != CSSOM_SHEETS)
		return 0;
	indexed = cssom_key(key, &index);
	if (!indexed)
		return 0;
	element = cssom_index(state->document, index);
	if (element == NULL)
		return 0;

	/* Source and actual Document roots in the factory retain this current query during allocations. */
	status = cssom_wrap(NULL, element, &property->temporary);
	if (status != 0)
		return -status;
	property->holder = object;
	property->value = &property->temporary;
	property->attributes = VM_PROPERTY_CONFIGURABLE | VM_PROPERTY_ENUMERABLE;

	/* Succeeded: the native descriptor exposes the actual current source wrapper. */
	return 1;
}

/* Enumerates current supported indices before the VM appends ordinary string and symbol expandos. */
static int
cssom_own_keys(
	struct vm_heap *heap,
	struct vm_object *object,
	struct wb_vector *keys)
{
	struct cssom_state *state;
	struct vm_cell *root;
	vm_value key;
	char text[11];
	uint32_t count;
	uint32_t index;
	int printed;
	int status;

	/* Only the live Document query exposes supported indexed properties. */
	state = (struct cssom_state *)vm_value_as_cell(object->internal);
	if (state->kind != CSSOM_SHEETS)
		return 0;
	status = cssom_count(state->document, &count);
	if (status != 0)
		return status;

	/* Protect the actual query while large numeric key atoms allocate. */
	root = &object->cell;
	status = vm_heap_add_root(heap, &root);
	if (status != 0)
		return status;
	for (index = 0; index < count; index++) {
		/* Small keys use immediate indices; larger supported keys use exact decimal atoms. */
		if (index <= INT32_MAX) {
			key = vm_value_int32((int32_t)index);
		} else {
			printed = snprintf(text, sizeof(text), "%u", (unsigned)index);
			if (printed < 0 || (size_t)printed >= sizeof(text)) {
				vm_heap_remove_root(heap, &root);
				return EINVAL;
			}

			/* Atom construction must retain the native query and every actual owner edge. */
			key = vm_key_from_ascii(heap, text);
			if (key == VM_VALUE_EMPTY) {
				vm_heap_remove_root(heap, &root);
				return ENOMEM;
			}
		}

		/* Current keys precede ordinary properties appended by the VM. */
		status = wb_vector_push(keys, &key);
		if (status != 0) {
			vm_heap_remove_root(heap, &root);
			return status;
		}
	}

	/* Key enumeration no longer needs its temporary actual query root. */
	vm_heap_remove_root(heap, &root);

	/* Succeeded: current native key order agrees with indexed membership. */
	return 0;
}

/* Refuses canonical indexed definitions while preserving genuine ordinary expandos. */
static int
cssom_define(
	struct vm_realm *realm,
	struct vm_object *object,
	vm_value key,
	const struct vm_descriptor *descriptor,
	int *handled,
	int *done)
{
	struct cssom_state *state;
	uint32_t index;
	int indexed;

	UNUSED_PARAMETER(realm);
	UNUSED_PARAMETER(descriptor);

	/* Other wrapper kinds permit ordinary extensible object properties. */
	*handled = 0;
	*done = 0;
	state = (struct cssom_state *)vm_value_as_cell(object->internal);
	if (state->kind != CSSOM_SHEETS)
		return 0;
	indexed = cssom_key(key, &index);
	if (indexed)
		*handled = 1;

	/* Succeeded: no indexed setter can replace actual current native membership. */
	return 0;
}

/* Preserves currently supported indices and permits deletion of missing indices and expandos. */
static int
cssom_delete(
	struct vm_heap *heap,
	struct vm_object *object,
	vm_value key,
	int *handled,
	int *deleted)
{
	struct cssom_state *state;
	struct dom_element *element;
	uint32_t index;
	int indexed;

	UNUSED_PARAMETER(heap);

	/* Ordinary sheet and rule-count properties use the VM's normal deletion algorithm. */
	*handled = 0;
	*deleted = 1;
	state = (struct cssom_state *)vm_value_as_cell(object->internal);
	if (state->kind != CSSOM_SHEETS)
		return 0;
	indexed = cssom_key(key, &index);
	if (!indexed)
		return 0;

	/* Current supported membership decides deletion after any previous DOM changes. */
	*handled = 1;
	element = cssom_index(state->document, index);
	if (element != NULL)
		*deleted = 0;

	/* Succeeded: virtual native members cannot be removed by ordinary property deletion. */
	return 0;
}

/* Keeps a live indexed query extensible while ordinary sheet and count wrappers follow VM semantics. */
static int
cssom_prevent_extensions(
	struct vm_object *object,
	int *allowed)
{
	struct cssom_state *state;

	/* Actual live membership could otherwise add supported properties after extension was prevented. */
	state = (struct cssom_state *)vm_value_as_cell(object->internal);
	*allowed = state->kind != CSSOM_SHEETS;

	/* Succeeded: only the native legacy indexed query refuses preventing extensions. */
	return 0;
}

/* Validates ordinary rule syntax, current index and import ordering before actual source mutation. */
static int
cssom_insert_text(
	struct cssom_state *state,
	const uint16_t *units,
	size_t length,
	uint32_t index)
{
	struct css_rule_model *probe;
	const uint16_t *source;
	size_t source_length;
	size_t count;
	size_t offset;
	uint32_t id;
	int type;
	int present;
	int status;

	/* Public insertion parses the rule before returning a list index or hierarchy error. */
	probe = NULL;
	status = css_rule_model_create(&probe, state->document->heap, NULL, 0);
	if (status != 0)
		return status;
	status = css_rule_model_insert(probe, units, length, 0);
	css_rule_model_destroy(probe);
	if (status != 0)
		return status;

	/* Ordinary rules cannot move any actual following top-level import after a style rule. */
	count = css_rule_model_count(state->source->model);
	if (index > count)
		return ERANGE;
	for (offset = index; offset < count; offset++) {
		id = css_rule_model_id(state->source->model, offset);
		status = css_rule_model_source(state->source->model, id, &source, &source_length, &type, &present);
		if (status != 0)
			return status;
		if (type == 3)
			return EACCES;
	}

	/* Succeeded or failed: the existing model commits atomically only after every prior check. */
	status = css_rule_model_insert(state->source->model, units, length, index);
	return status;
}

/* Maps expected native mutation failures to actual DOM exceptions without changing source state. */
static int
cssom_mutation_error(
	struct vm_realm *realm,
	int status)
{
	/* Syntax and bounds are native parser or live model outcomes, never inferred from DOM Text. */
	if (status == EINVAL) {
		status = bind_throw_dom(realm, "SyntaxError", "The input is not one supported CSS rule.");
		return status;
	}

	/* Current list bounds determine the index exception. */
	if (status == ERANGE) {
		status = bind_throw_dom(realm, "IndexSizeError", "The rule index is outside the current rule list.");
		return status;
	}

	/* Existing imports must remain before ordinary rules in the actual source ordering. */
	if (status == EACCES) {
		status = bind_throw_dom(realm, "HierarchyRequestError", "A style rule cannot precede an import rule.");
		return status;
	}

	/* This bounded core explicitly refuses unimplemented at-rule insertion. */
	if (status == ENOTSUP) {
		status = bind_throw_dom(realm, "NotSupportedError", "At-rule insertion is not implemented in this partial CSSOM core.");
		return status;
	}

	/* Unhandled resource failures retain their original native error result. */
	return status;
}
