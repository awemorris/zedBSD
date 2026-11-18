/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Native boundaries retain actual nodes and repair them through weak DOM mutation delivery. */

#include "bind/internal.h"

#include <errno.h>
#include <stdint.h>

/* Shared mutation actions keep typed argument conversion and root cleanup in one implementation. */
enum range_action {
	RANGE_START,
	RANGE_END,
	RANGE_START_BEFORE,
	RANGE_START_AFTER,
	RANGE_END_BEFORE,
	RANGE_END_AFTER,
	RANGE_SELECT_NODE,
	RANGE_SELECT_CONTENTS
};

/* Each inherited accessor reads genuine native boundary state rather than visible expandos. */
enum range_field {
	RANGE_START_CONTAINER,
	RANGE_END_CONTAINER,
	RANGE_START_OFFSET,
	RANGE_END_OFFSET,
	RANGE_COLLAPSED,
	RANGE_ANCESTOR
};

/* A boundary counts UTF16 units in character data or ordinary children in other containers. */
struct range_point {
	struct dom_node *node;
	uint32_t offset;
};

/* Each native range retains its creator Document and both endpoint graphs without a raw realm. */
struct range_state {
	struct vm_cell cell;
	struct dom_document *document;
	struct range_point start;
	struct range_point end;
	struct dom_removal_subscription *subscription;
};

/* One immutable contents interval refers only to source and destination graphs retained by callee roots. */
struct range_clone_frame {
	struct range_point start;
	struct range_point end;
	struct dom_node *output;
	struct dom_node *next;
};

/* An extraction frame snapshots its first and after-last original child before moves renumber siblings. */
struct range_extract_frame {
	struct range_point start;
	struct range_point end;
	struct dom_node *output;
	struct dom_node *next;
	struct dom_node *stop;
};

static int range_surround(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int range_surround_native(struct vm_realm *realm, vm_value receiver, struct range_state *state, struct dom_node *parent, struct vm_cell **roots);
static int range_surround_partial(struct vm_realm *realm, struct range_state *state);
static int range_extract_contents(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int range_extract_interval(struct vm_realm *realm, struct range_state *state, struct range_point start, struct range_point end, struct dom_node *output, struct vm_cell **roots);
static int range_extract_prepare(struct vm_realm *realm, struct range_point start, struct range_point end, struct dom_node *output, struct range_extract_frame *frame);
static int range_extract_collapse(struct range_state *state, struct range_point start, struct range_point end);
static int range_extract_move(struct vm_realm *realm, struct dom_node *node, struct dom_node *output, struct vm_cell **pending);
static int range_delete_contents(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int range_delete_apply(struct vm_realm *realm, struct range_state *state, struct vm_cell **roots);
static int range_delete_collect(struct range_point start, struct range_point end, struct wb_vector *nodes);
static struct dom_node *range_after_subtree(struct dom_node *root, struct dom_node *node);
static int range_clone_contents(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int range_clone_slice(struct vm_realm *realm, struct dom_node *source, uint32_t begin, uint32_t end, struct dom_node *output, struct vm_cell **pending);
static int range_contained(struct dom_node *node, struct range_point start, struct range_point end);
static int range_clone_prepare(struct vm_realm *realm, struct range_point start, struct range_point end, struct dom_node *output, struct range_clone_frame *frame, struct vm_cell **pending);
static int range_clone_interval(struct vm_realm *realm, struct range_point start, struct range_point end, struct dom_node *output, struct vm_cell **pending);
static void range_trace(struct vm_heap *heap, struct vm_cell *cell);
static void range_finalize(struct vm_heap *heap, struct vm_cell *cell);
static int range_commit(struct range_state *state, struct range_point start, struct range_point end);
static void range_text_split(void *context, struct dom_node *node, struct dom_node *created, uint32_t offset);
static void range_repair_split(struct range_point *point, struct dom_node *node, struct dom_node *created, struct dom_node *parent, uint32_t offset, uint32_t index);
static int range_insert(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int range_insert_native(struct vm_realm *realm, struct range_state *state, struct dom_node *node, struct vm_cell **roots);
static void range_removed(void *context, struct dom_node *node);
static void range_inserted(void *context, struct dom_node *node);
static void range_data_changed(void *context, struct dom_node *node, size_t offset, size_t removed, size_t added);
static void range_repair_data(struct range_point *point, struct dom_node *node, size_t offset, size_t removed, size_t added);
static void range_repair_point(struct range_point *point, struct dom_node *node, struct dom_node *parent, uint32_t index);
static int range_create(struct vm_realm *realm, struct dom_document *document, vm_value *result);
static int range_this(struct vm_realm *realm, vm_value receiver, struct range_state **out);
static int range_construct(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int range_read(struct vm_realm *realm, vm_value receiver, enum range_field field, vm_value *result);
static int range_change(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, enum range_action action, vm_value *result);
static int range_apply(struct vm_realm *realm, struct range_state *state, struct dom_node *node, uint32_t offset, enum range_action action);
static int range_execute(struct vm_realm *realm, struct range_state *state, struct dom_node *node, const vm_value *args, unsigned count, enum range_action action);
static struct dom_node *range_root(struct dom_node *node);
static struct dom_node *range_ancestor(struct dom_node *left, struct dom_node *right);
static size_t range_length(struct dom_node *node);
static size_t range_index(struct dom_node *node);
static int range_compare(struct range_point left, struct range_point right);
static void range_unroot(struct vm_heap *heap, struct vm_cell **roots, unsigned count);
static int range_startContainer(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int range_endContainer(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int range_startOffset(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int range_endOffset(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int range_collapsed(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int range_commonAncestorContainer(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int range_setStart(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int range_setEnd(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int range_setStartBefore(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int range_setStartAfter(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int range_setEndBefore(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int range_setEndAfter(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int range_selectNode(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int range_selectNodeContents(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int range_collapse(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int range_detach(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int range_clone(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int range_compare_boundaries(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int range_stringify(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int range_append_text(struct range_state *state, struct dom_character_data *text, struct wb_units *units);
static struct dom_node *range_next_node(struct dom_node *root, struct dom_node *node);

/* Each collectible native state owns one weak token until its arbitrary-order finalization. */
static const struct vm_cell_type range_type = { "range-state", range_trace, range_finalize };

/* Boundary core accessors are inherited by the concrete native Range interface. */
static const struct bind_attribute abstract_range_attributes[] = {
	{ "startContainer", range_startContainer, NULL },
	{ "endContainer", range_endContainer, NULL },
	{ "startOffset", range_startOffset, NULL },
	{ "endOffset", range_endOffset, NULL },
	{ "collapsed", range_collapsed, NULL },
	{ NULL, NULL, NULL }
};

/* The concrete range computes the common ordinary ancestor of its native endpoint containers. */
static const struct bind_attribute range_attributes[] = {
	{ "commonAncestorContainer", range_commonAncestorContainer, NULL },
	{ NULL, NULL, NULL }
};

/* Native boundary, comparison, stringification and concrete insertion operations share genuine Range state. */
static const struct bind_operation range_operations[] = {
	{ "setStart", 2, range_setStart },
	{ "setEnd", 2, range_setEnd },
	{ "setStartBefore", 1, range_setStartBefore },
	{ "setStartAfter", 1, range_setStartAfter },
	{ "setEndBefore", 1, range_setEndBefore },
	{ "setEndAfter", 1, range_setEndAfter },
	{ "selectNode", 1, range_selectNode },
	{ "selectNodeContents", 1, range_selectNodeContents },
	{ "collapse", 0, range_collapse },
	{ "detach", 0, range_detach },
	{ "cloneRange", 0, range_clone },
	{ "compareBoundaryPoints", 2, range_compare_boundaries },
	{ "toString", 0, range_stringify },
	{ "insertNode", 1, range_insert },
	{ "cloneContents", 0, range_clone_contents },
	{ "deleteContents", 0, range_delete_contents },
	{ "extractContents", 0, range_extract_contents },
	{ "surroundContents", 1, range_surround },
	{ NULL, 0, NULL }
};

/* Historical comparison mode constants remain readonly on constructor and prototype. */
static const struct bind_constant range_constants[] = {
	{ "START_TO_START", 0 },
	{ "START_TO_END", 1 },
	{ "END_TO_END", 2 },
	{ "END_TO_START", 3 },
	{ NULL, 0 }
};

/* AbstractRange can only be supplied through a genuine concrete native implementation. */
const struct bind_interface bind_abstract_range_interface = {
	"AbstractRange", BIND_NO_PARENT, 0, NULL, abstract_range_attributes, NULL, NULL
};

/* Range constructors associate an initial document boundary with their actual current Window. */
const struct bind_interface bind_range_interface = {
	"Range", BIND_ABSTRACT_RANGE, 0, range_construct, range_attributes, range_operations, range_constants
};

/*
 * Creates a native range initially collapsed at its actual receiver Document.
 */
int
bind_create_range(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_node *node;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* A real Document identity precedes prototype selection and every native allocation. */
	status = bind_this_node(realm, receiver, &node);
	if (status != 0)
		return status;

	/* Ordinary nodes cannot supply a Document factory receiver. */
	if (node->type != DOM_DOCUMENT) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* The factory belongs to the receiver Document rather than the method borrower's Window. */
	status = range_create(realm, (struct dom_document *)node, result);
	if (status != 0)
		return status;

	/* Succeeded: an independent native range retains its actual creator and endpoint graph. */
	return 0;
}

/* Surrounds a genuine native interval while retaining every participant through host GC. */
static int
range_surround(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct range_state *state;
	struct dom_node *parent;
	struct dom_node *source;
	struct vm_cell *roots[8];
	unsigned index;
	int status;

	/* Receiver branding precedes required typed argument conversion. */
	*result = VM_VALUE_UNDEFINED;
	status = range_this(realm, receiver, &state);
	if (status != 0)
		return status;

	/* A missing parent cannot enter the native surround algorithm. */
	if (count < 1U) {
		status = vm_throw_type_error(realm, "Range.surroundContents requires a Node.");
		return status;
	}

	/* Establish actual native identity without script-visible property conversion. */
	status = bind_argument_node(realm, args[0], &parent);
	if (status != 0)
		return status;

	/* Foreign collector cells cannot participate in this graph. */
	if (parent->document->heap != realm->heap)
		return EINVAL;

	/* Slots zero through four satisfy the existing native Range insertion contract. */
	source = range_root(state->start.node);
	roots[0] = &state->cell;
	roots[1] = &parent->cell;
	roots[2] = &state->start.node->cell;
	roots[3] = NULL;
	roots[4] = NULL;
	roots[5] = NULL;
	roots[6] = NULL;
	roots[7] = &source->cell;
	for (index = 0; index < 8U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0) {
			range_unroot(realm->heap, roots, index);
			return status;
		}
	}

	/* Output ownership stays separate from insertion scratch and current removed-child slots. */
	status = range_surround_native(realm, receiver, state, parent, roots);
	range_unroot(realm->heap, roots, 8);
	if (status != 0)
		return status;

	/* Succeeded: the actual parent encloses the selected content and is itself selected. */
	return 0;
}

/* Executes normative surround ordering using the existing native mutation operations. */
static int
range_surround_native(
	struct vm_realm *realm,
	vm_value receiver,
	struct range_state *state,
	struct dom_node *parent,
	struct vm_cell **roots)
{
	struct dom_node *fragment;
	struct dom_node *child;
	struct bind_window *window;
	vm_value extracted;
	int status;

	/* Partial non-Text refusal precedes invalid parent kind checks and extraction. */
	status = range_surround_partial(realm, state);
	if (status != 0)
		return status;

	/* CharacterData parents deliberately reach the later native insertion or append refusal. */
	if (parent->type == DOM_DOCUMENT ||
	    parent->type == DOM_DOCUMENT_TYPE ||
	    parent->type == DOM_DOCUMENT_FRAGMENT) {
		status = bind_throw_dom(realm, "InvalidNodeTypeError", "This node kind cannot surround Range content.");
		return status;
	}

	/* Extraction supplies actual moved identities and native live boundary repair. */
	status = range_extract_contents(realm, receiver, NULL, 0, &extracted);
	if (status != 0)
		return status;
	fragment = bind_node_of(extracted);
	if (fragment == NULL)
		return EINVAL;

	/* The output remains alive even when insertion overwrites its reference and parent scratch roots. */
	roots[5] = &fragment->cell;
	window = bind_window_of(realm);
	if (parent->document->view != NULL)
		window = parent->document->view;

	/* Clear ordinary children using real removal, frame retirement, weak repair and owner notices. */
	while (parent->first_child != NULL) {
		child = parent->first_child;
		roots[6] = &child->cell;
		dom_remove(child);
		status = bind_environment_child_mutation(window, parent, NULL, child);
		if (status != 0)
			return status;
		roots[6] = NULL;
	}

	/* The current start may differ after extraction and clearing an ancestor parent. */
	roots[2] = &state->start.node->cell;
	status = range_insert_native(realm, state, parent, roots);
	if (status != 0)
		return status;

	/* Append validates cycles and parent kind before transferring all extracted original identities. */
	status = bind_insert(realm, parent, fragment, NULL);
	if (status != 0)
		return status;

	/* Final selection uses the actual post-insertion parent and index, never visible point fields. */
	status = range_execute(realm, state, parent, NULL, 0, RANGE_SELECT_NODE);
	if (status != 0)
		return status;

	/* Succeeded: all seven surround steps completed in their observable order. */
	return 0;
}

/* Rejects native endpoint ancestor branches that are partially contained and not Text. */
static int
range_surround_partial(
	struct vm_realm *realm,
	struct range_state *state)
{
	struct dom_node *ancestor;
	struct dom_node *node;
	int status;

	/* Only ancestors on one endpoint path below the common ancestor are partially contained. */
	ancestor = range_ancestor(state->start.node, state->end.node);
	if (ancestor == NULL)
		return EINVAL;

	/* Text is the only permitted partially contained node kind on the start path. */
	for (node = state->start.node; node != ancestor; node = node->parent) {
		if (node->type != DOM_TEXT && node->type != DOM_CDATA_SECTION) {
			status = bind_throw_dom(realm, "InvalidStateError", "A non-Text node is partially contained in this Range.");
			return status;
		}
	}

	/* The end path has the same rule, including partial Comment refusal. */
	for (node = state->end.node; node != ancestor; node = node->parent) {
		if (node->type != DOM_TEXT && node->type != DOM_CDATA_SECTION) {
			status = bind_throw_dom(realm, "InvalidStateError", "A non-Text node is partially contained in this Range.");
			return status;
		}
	}

	/* Succeeded: every partial branch is Text and the source is still unchanged. */
	return 0;
}

/* Extracts a genuine native interval into a Fragment of its current start Document. */
static int
range_extract_contents(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct range_state *state;
	struct dom_node *fragment;
	struct vm_cell *roots[6];
	struct bind_window *window;
	unsigned index;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The genuine private Range brand is checked before any output allocation. */
	status = range_this(realm, receiver, &state);
	if (status != 0)
		return status;

	/* Original endpoints survive collapse, and the complete source root owns unvisited nodes. */
	roots[0] = &state->cell;
	roots[1] = &state->start.node->cell;
	roots[2] = &state->end.node->cell;
	roots[3] = &range_root(state->start.node)->cell;
	roots[4] = NULL;
	roots[5] = NULL;
	for (index = 0; index < 6U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0) {
			range_unroot(realm->heap, roots, index);
			return status;
		}
	}

	/* The actual current start owner, not Range creator or calling realm, owns the Fragment. */
	fragment = dom_fragment_create(state->start.node->document);
	if (fragment == NULL) {
		range_unroot(realm->heap, roots, 6);
		return ENOMEM;
	}

	/* The output root owns all subsequent shells and moved native children. */
	roots[4] = &fragment->cell;

	/* Equivalent nested intervals transfer original identities into the rooted output graph. */
	status = range_extract_interval(realm, state, state->start, state->end, fragment, roots);
	if (status != 0) {
		range_unroot(realm->heap, roots, 6);
		return status;
	}

	/* Only a complete Fragment gets wrapped and published in the actual owner realm. */
	window = bind_window_of(realm);
	status = bind_wrap(window, fragment, result);
	range_unroot(realm->heap, roots, 6);
	if (status != 0)
		return status;

	/* Succeeded: full selected content owns moved nodes and partial independent shells. */
	return 0;
}

/* Transfers a supported interval through checked iterative partial-branch frames. */
static int
range_extract_interval(
	struct vm_realm *realm,
	struct range_state *state,
	struct range_point start,
	struct range_point end,
	struct dom_node *output,
	struct vm_cell **roots)
{
	struct wb_vector frames;
	struct range_extract_frame initial;
	struct range_extract_frame child_frame;
	struct range_extract_frame *frame;
	struct range_point child_start;
	struct range_point child_end;
	struct dom_node *child;
	struct dom_node *copy;
	size_t length;
	int contains_start;
	int contains_end;
	int status;

	/* A collapsed Range returns the already-created empty Fragment without source work. */
	if (start.node == end.node && start.offset == end.offset)
		return 0;

	/* One native CharacterData container produces an exact copy before data deletion. */
	if (start.node == end.node &&
	    (start.node->type == DOM_TEXT ||
	        start.node->type == DOM_CDATA_SECTION ||
	        start.node->type == DOM_PROCESSING_INSTRUCTION ||
	        start.node->type == DOM_COMMENT)) {
		status = range_clone_slice(realm, start.node, start.offset, end.offset, output, &roots[5]);
		if (status != 0)
			return status;
		status = dom_text_replace(start.node, start.offset, end.offset - start.offset, NULL, 0);
		if (status != 0)
			return status;

		/* Succeeded: the ordinary data hook collapses the live Range in its own container. */
		return 0;
	}

	/* Preflight doctype and frame storage before original Range collapse or source mutation. */
	wb_vector_init(&frames, sizeof(struct range_extract_frame));
	status = range_extract_prepare(realm, start, end, output, &initial);
	if (status != 0) {
		wb_vector_release(&frames);
		return status;
	}

	/* Allocate the initial frame before the original Range point is changed. */
	status = wb_vector_push(&frames, &initial);
	if (status != 0) {
		wb_vector_release(&frames);
		return status;
	}

	/* Publish the normative collapse before partial data deletion or moving any source node. */
	status = range_extract_collapse(state, start, end);
	if (status != 0) {
		wb_vector_release(&frames);
		return status;
	}

	/* Each parent resumes its next original sibling after a partial child interval finishes. */
	while (frames.count != 0) {
		frame = wb_vector_at(&frames, frames.count - 1U);
		child = frame->next;
		if (child == NULL || child == frame->stop) {
			wb_vector_pop(&frames);
			continue;
		}

		/* Save the next sibling before a move, checked push or nested interval can change links. */
		frame->next = child->next;
		contains_start = dom_is_inclusive_ancestor(child, frame->start.node);
		contains_end = dom_is_inclusive_ancestor(child, frame->end.node);
		if (contains_start || contains_end) {
			/* Native child length bounds every equivalent recursive interval. */
			length = range_length(child);
			if (length > UINT32_MAX) {
				wb_vector_release(&frames);
				return EINVAL;
			}

			/* Immutable original endpoints select the complete suffix or prefix in this branch. */
			child_start.node = child;
			child_start.offset = 0;
			if (contains_start)
				child_start = frame->start;
			child_end.node = child;
			child_end.offset = (uint32_t)length;
			if (contains_end)
				child_end = frame->end;

			/* CharacterData is copied with exact code units, then shortened in the source. */
			if (child->type == DOM_TEXT ||
			    child->type == DOM_CDATA_SECTION ||
			    child->type == DOM_PROCESSING_INSTRUCTION ||
			    child->type == DOM_COMMENT) {
				status = range_clone_slice(realm, child, child_start.offset, child_end.offset, frame->output, &roots[5]);
				if (status != 0) {
					wb_vector_release(&frames);
					return status;
				}

				/* The native original retains only unselected code units after the copied slice exists. */
				status = dom_text_replace(child, child_start.offset, child_end.offset - child_start.offset, NULL, 0);
				if (status != 0) {
					wb_vector_release(&frames);
					return status;
				}

				/* This exact partial slice has no descendant frame. */
				continue;
			}

			/* A shallow genuine native shell retains attributes/control state without source children. */
			status = bind_clone_node(realm, child, 0, &copy);
			if (status != 0) {
				wb_vector_release(&frames);
				return status;
			}

			/* The complete shell joins the output graph before its nested selected interval. */
			roots[5] = &copy->cell;
			dom_append_child(frame->output, copy);
			status = range_extract_prepare(realm, child_start, child_end, copy, &child_frame);
			if (status != 0) {
				wb_vector_release(&frames);
				return status;
			}

			/* Checked growth may relocate the parent frame, whose next sibling is already saved. */
			status = wb_vector_push(&frames, &child_frame);
			if (status != 0) {
				wb_vector_release(&frames);
				return status;
			}

			/* The copied branch is rooted by output while this parent resumes afterward. */
			continue;
		}

		/* Every remaining child inside the frozen interval was fully contained before earlier moves changed indexes. */
		status = range_extract_move(realm, child, frame->output, &roots[5]);
		if (status != 0) {
			wb_vector_release(&frames);
			return status;
		}
	}

	/* Every immutable frame has reached its final original sibling. */
	wb_vector_release(&frames);

	/* Succeeded: output contains copied partial shells and original complete subtrees. */
	return 0;
}

/* Preflights a direct interval's doctype and representable native child boundaries. */
static int
range_extract_prepare(
	struct vm_realm *realm,
	struct range_point start,
	struct range_point end,
	struct dom_node *output,
	struct range_extract_frame *frame)
{
	struct dom_node *ancestor;
	struct dom_node *child;
	size_t length;
	size_t start_length;
	size_t end_length;
	int contained;
	int contains_start;
	int contains_end;
	int status;

	/* Immutable frame points remain separate from the original Range's live collapse. */
	frame->start = start;
	frame->end = end;
	frame->output = output;
	frame->next = NULL;
	frame->stop = NULL;
	start_length = range_length(start.node);
	end_length = range_length(end.node);
	if (start_length > UINT32_MAX || end_length > UINT32_MAX ||
	    start.offset > start_length || end.offset > end_length)
		return EINVAL;

	/* Both immutable endpoints must still share one ordinary source tree. */
	ancestor = range_ancestor(start.node, end.node);
	if (ancestor == NULL)
		return EINVAL;

	/* A whole contained doctype cannot enter an output DocumentFragment. */
	for (child = ancestor->first_child; child != NULL; child = child->next) {
		length = range_length(child);
		if (length > UINT32_MAX)
			return EINVAL;
		contained = range_contained(child, start, end);
		if (contained && child->type == DOM_DOCUMENT_TYPE) {
			status = bind_throw_dom(realm, "HierarchyRequestError", "Range extraction cannot contain a DocumentType.");
			return status;
		}

		/* Freeze the selected sibling interval before source removals renumber its indexes. */
		contains_start = dom_is_inclusive_ancestor(child, start.node);
		contains_end = dom_is_inclusive_ancestor(child, end.node);
		if (contained || contains_start || contains_end) {
			if (frame->next == NULL)
				frame->next = child;
			frame->stop = child->next;
		}
	}

	/* Succeeded: this original interval can be traversed without flattening identities. */
	return 0;
}

/* Collapses the original Range at the DOM-specified point before any source mutation. */
static int
range_extract_collapse(
	struct range_state *state,
	struct range_point start,
	struct range_point end)
{
	struct range_point point;
	struct dom_node *reference;
	struct dom_node *parent;
	size_t index;
	int ancestor;
	int status;

	/* A start container enclosing the end keeps its original boundary tuple. */
	point = start;
	ancestor = dom_is_inclusive_ancestor(start.node, end.node);
	if (!ancestor) {
		/* Find the first start-side branch directly below the shared ancestor. */
		reference = start.node;
		while (reference->parent != NULL) {
			ancestor = dom_is_inclusive_ancestor(reference->parent, end.node);
			if (ancestor)
				break;
			reference = reference->parent;
		}

		/* Unsupported disconnected graphs cannot supply a surrounding collapse point. */
		parent = reference->parent;
		if (parent == NULL)
			return EINVAL;
		index = range_index(reference);
		if (index >= UINT32_MAX)
			return EINVAL;

		/* The original parent position just after this branch precedes all source mutations. */
		point.node = parent;
		point.offset = (uint32_t)(index + 1U);
	}

	/* Subscription preparation is checked before either native point changes. */
	status = range_commit(state, point, point);
	if (status != 0)
		return status;

	/* Succeeded: later weak repairs may adjust the one collapsed tuple together. */
	return 0;
}

/* Moves one complete native identity after notifying its actual source owner. */
static int
range_extract_move(
	struct vm_realm *realm,
	struct dom_node *node,
	struct dom_node *output,
	struct vm_cell **pending)
{
	struct dom_node *parent;
	struct bind_window *window;
	int status;

	/* A selected node must still belong to its captured source parent. */
	parent = node->parent;
	if (parent == NULL)
		return EINVAL;

	/* The pending slot protects a detached node through frame teardown and record allocation. */
	*pending = &node->cell;
	window = bind_window_of(realm);
	if (parent->document->view != NULL)
		window = parent->document->view;
	dom_remove(node);
	status = bind_environment_child_mutation(window, parent, NULL, node);
	if (status != 0)
		return status;

	/* The rooted output Fragment owns this original identity after native insertion. */
	dom_append_child(output, node);
	*pending = NULL;

	/* Succeeded: the source observer has the removed identity and output has the node. */
	return 0;
}

/* Deletes a genuine private Range interval through the ordinary native DOM mutation path. */
static int
range_delete_contents(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct range_state *state;
	struct vm_cell *roots[5];
	unsigned index;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Branding is checked before any result publication or source mutation. */
	status = range_this(realm, receiver, &state);
	if (status != 0)
		return status;

	/* The original endpoint graphs and ordinary source root outlive live point collapse. */
	roots[0] = &state->cell;
	roots[1] = &state->start.node->cell;
	roots[2] = &state->end.node->cell;
	roots[3] = NULL;
	roots[4] = &range_root(state->start.node)->cell;
	for (index = 0; index < 5U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0) {
			range_unroot(realm->heap, roots, index);
			return status;
		}
	}

	/* Native removal and observer allocation run under callee-owned participants. */
	status = range_delete_apply(realm, state, roots);
	range_unroot(realm->heap, roots, 5);
	if (status != 0)
		return status;

	/* Succeeded: deletion has no returned content. */
	*result = VM_VALUE_UNDEFINED;
	return 0;
}

/* Applies the original interval's deletion order while retaining each detached node. */
static int
range_delete_apply(
	struct vm_realm *realm,
	struct range_state *state,
	struct vm_cell **roots)
{
	struct wb_vector nodes;
	struct range_point start;
	struct range_point end;
	struct range_point collapse;
	struct dom_node *reference;
	struct dom_node *node;
	struct dom_node *parent;
	struct bind_window *window;
	struct dom_node **entry;
	size_t index;
	size_t start_length;
	size_t end_length;
	int ancestor;
	int status;

	/* Collapsed intervals do not allocate, notify or change native generations. */
	start = state->start;
	end = state->end;
	if (start.node == end.node && start.offset == end.offset)
		return 0;

	/* The same CharacterData container deletes one exact selected UTF16 interval. */
	if (start.node == end.node &&
	    (start.node->type == DOM_TEXT ||
	        start.node->type == DOM_CDATA_SECTION ||
	        start.node->type == DOM_PROCESSING_INSTRUCTION ||
	        start.node->type == DOM_COMMENT)) {
		status = dom_text_replace(start.node, start.offset, end.offset - start.offset, NULL, 0);
		if (status != 0)
			return status;

		/* Succeeded: the existing weak data repair collapses every affected live boundary. */
		return 0;
	}

	/* Collect topmost contained nodes before any collapse or removed link changes traversal. */
	wb_vector_init(&nodes, sizeof(struct dom_node *));
	status = range_delete_collect(start, end, &nodes);
	if (status != 0) {
		wb_vector_release(&nodes);
		return status;
	}

	/* Reject unsupported or stale partial data points before publishing any collapse. */
	start_length = range_length(start.node);
	end_length = range_length(end.node);
	if (start_length > UINT32_MAX || end_length > UINT32_MAX ||
	    start.offset > start_length || end.offset > end_length) {
		wb_vector_release(&nodes);
		return EINVAL;
	}

	/* The original start remains the collapse point when it contains the end. */
	collapse = start;
	ancestor = dom_is_inclusive_ancestor(start.node, end.node);
	if (!ancestor) {
		/* Find the first start-side branch directly below the shared ancestor. */
		reference = start.node;
		while (reference->parent != NULL) {
			ancestor = dom_is_inclusive_ancestor(reference->parent, end.node);
			if (ancestor)
				break;
			reference = reference->parent;
		}

		/* A malformed or unsupported disconnected graph must fail before native mutation. */
		parent = reference->parent;
		if (parent == NULL) {
			wb_vector_release(&nodes);
			return EINVAL;
		}

		/* The point just after that branch is representable in the existing Range offset type. */
		index = range_index(reference);
		if (index >= UINT32_MAX) {
			wb_vector_release(&nodes);
			return EINVAL;
		}

		/* The collapse keeps the next sibling position in the original parent. */
		collapse.node = parent;
		collapse.offset = (uint32_t)(index + 1U);
	}

	/* The DOM algorithm publishes the collapsed Range before any partial data or child mutation. */
	status = range_commit(state, collapse, collapse);
	if (status != 0) {
		wb_vector_release(&nodes);
		return status;
	}

	/* Remove the original start suffix while its native UTF16 buffer and points still exist. */
	if (start.node->type == DOM_TEXT ||
	    start.node->type == DOM_CDATA_SECTION ||
	    start.node->type == DOM_PROCESSING_INSTRUCTION ||
	    start.node->type == DOM_COMMENT) {
		status = dom_text_replace(start.node, start.offset, start_length - start.offset, NULL, 0);
		if (status != 0) {
			wb_vector_release(&nodes);
			return status;
		}
	}

	/* Each snapshot node is detached in original tree order and reported after complete unlink. */
	for (index = 0; index < nodes.count; index++) {
		entry = wb_vector_at(&nodes, index);
		node = *entry;
		parent = node->parent;
		if (parent == NULL) {
			wb_vector_release(&nodes);
			return EINVAL;
		}

		/* The removed graph remains alive through iframe teardown and observer allocations. */
		roots[3] = &node->cell;
		window = bind_window_of(realm);
		if (parent->document->view != NULL)
			window = parent->document->view;
		dom_remove(node);
		status = bind_environment_child_mutation(window, parent, NULL, node);
		if (status != 0) {
			wb_vector_release(&nodes);
			return status;
		}

		/* A complete observer record now owns any detached node it reports. */
		roots[3] = NULL;
	}

	/* Remove the original end prefix after contained children have detached. */
	if (end.node->type == DOM_TEXT ||
	    end.node->type == DOM_CDATA_SECTION ||
	    end.node->type == DOM_PROCESSING_INSTRUCTION ||
	    end.node->type == DOM_COMMENT) {
		status = dom_text_replace(end.node, 0, end.offset, NULL, 0);
		if (status != 0) {
			wb_vector_release(&nodes);
			return status;
		}
	}

	/* The C snapshot is no longer needed once all original nodes have been removed. */
	wb_vector_release(&nodes);

	/* Succeeded: the current collapsed Range reflects all live native repairs. */
	return 0;
}

/* Collects only highest contained ancestors in original tree order using checked C storage. */
static int
range_delete_collect(
	struct range_point start,
	struct range_point end,
	struct wb_vector *nodes)
{
	struct dom_node *root;
	struct dom_node *node;
	size_t length;
	int contained;
	int status;

	/* The endpoints must still have one actual ordinary ancestor before any mutation. */
	root = range_ancestor(start.node, end.node);
	if (root == NULL)
		return EINVAL;

	/* Descend only partial branches; a contained ancestor owns its whole subtree removal. */
	node = root->first_child;
	while (node != NULL) {
		length = range_length(node);
		if (length > UINT32_MAX)
			return EINVAL;

		/* Whole contained descendants require only the highest ancestor in the snapshot. */
		contained = range_contained(node, start, end);
		if (contained) {
			status = wb_vector_push(nodes, &node);
			if (status != 0)
				return status;

			/* Skip descendants that move with their already selected ancestor. */
			node = range_after_subtree(root, node);
			continue;
		}

		/* Other branches may contain a later whole node even when this parent is partial. */
		if (node->first_child != NULL) {
			node = node->first_child;
			continue;
		}

		/* A leaf has no selected descendant, so resume at the next sibling branch. */
		node = range_after_subtree(root, node);
	}

	/* Succeeded: every selected node is independent of the others' parent links. */
	return 0;
}

/* Advances after an entire subtree without descending into nodes already selected for removal. */
static struct dom_node *
range_after_subtree(
	struct dom_node *root,
	struct dom_node *node)
{
	/* Ascend until a following sibling exists within the original common ancestor. */
	while (node != root) {
		if (node->next != NULL)
			return node->next;
		node = node->parent;
	}

	/* Succeeded: traversal of the ordinary common ancestor is complete. */
	return NULL;
}

/* Clones a genuine native interval into a Fragment belonging to its current start owner. */
static int
range_clone_contents(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct range_state *state;
	struct dom_node *fragment;
	struct vm_cell *roots[3];
	struct bind_window *window;
	unsigned index;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Private native state precedes allocation and ignores visible boundary properties. */
	status = range_this(realm, receiver, &state);
	if (status != 0)
		return status;

	/* State retains the complete original graph; output and pending copies retain new participants. */
	roots[0] = &state->cell;
	roots[1] = NULL;
	roots[2] = NULL;
	for (index = 0; index < 3U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0) {
			range_unroot(realm->heap, roots, index);
			return status;
		}
	}

	/* The actual current start Document supplies Fragment ownership independently of the creator. */
	fragment = dom_fragment_create(state->start.node->document);
	if (fragment == NULL) {
		range_unroot(realm->heap, roots, 3);
		return ENOMEM;
	}

	/* Retain every copied shell and linked descendant before further allocation. */
	roots[1] = &fragment->cell;
	status = range_clone_interval(realm, state->start, state->end, fragment, &roots[2]);
	if (status != 0) {
		range_unroot(realm->heap, roots, 3);
		return status;
	}

	/* Wrapping chooses the Fragment's actual owner and retains it through wrapper allocation. */
	window = bind_window_of(realm);
	status = bind_wrap(window, fragment, result);
	range_unroot(realm->heap, roots, 3);
	if (status != 0)
		return status;

	/* Succeeded: the caller receives a complete independent native content graph. */
	return 0;
}

/* Copies exact UTF16 selection into a distinct node of the source's genuine native kind. */
static int
range_clone_slice(
	struct vm_realm *realm,
	struct dom_node *source,
	uint32_t begin,
	uint32_t end,
	struct dom_node *output,
	struct vm_cell **pending)
{
	struct dom_node *copy;
	int status;

	/* The shared helper preserves native character kind and exact same-owner data. */
	status = bind_clone_node(realm, source, 0, &copy);
	if (status != 0)
		return status;
	*pending = &copy->cell;

	/* Prefix deletion and retained suffix truncation do not allocate after copying. */
	status = dom_text_replace(copy, 0, begin, NULL, 0);
	if (status != 0)
		return status;
	status = dom_text_truncate(copy, (size_t)end - begin);
	if (status != 0)
		return status;

	/* Even an empty selected slice retains its genuine Text or Comment node identity. */
	dom_append_child(output, copy);

	/* Succeeded: the selected exact code units belong to the linked independent copy. */
	return 0;
}

/* Identifies fully contained native nodes using strict boundary ordering in one ordinary tree. */
static int
range_contained(
	struct dom_node *node,
	struct range_point start,
	struct range_point end)
{
	struct range_point point;
	size_t length;
	int order;

	/* A node's first internal point must follow the interval's start. */
	point.node = node;
	point.offset = 0;
	order = range_compare(point, start);
	if (order <= 0)
		return 0;

	/* Supported interval graphs represent every native length with an unsigned boundary offset. */
	length = range_length(node);
	if (length > UINT32_MAX)
		return 0;
	point.offset = (uint32_t)length;
	order = range_compare(point, end);
	if (order >= 0)
		return 0;

	/* Succeeded: both internal boundary points lie strictly inside this native interval. */
	return 1;
}

/* Prepares one immutable interval after handling collapsed or same-character selections. */
static int
range_clone_prepare(
	struct vm_realm *realm,
	struct range_point start,
	struct range_point end,
	struct dom_node *output,
	struct range_clone_frame *frame,
	struct vm_cell **pending)
{
	struct dom_node *ancestor;
	struct dom_node *child;
	int contained;
	int status;

	/* Exhausted frames have no next child, including exact collapsed intervals. */
	frame->start = start;
	frame->end = end;
	frame->output = output;
	frame->next = NULL;
	if (start.node == end.node && start.offset == end.offset)
		return 0;

	/* CharacterData in one container is copied as an exact selected native slice. */
	if (start.node == end.node &&
	    (start.node->type == DOM_TEXT ||
	        start.node->type == DOM_CDATA_SECTION ||
	        start.node->type == DOM_PROCESSING_INSTRUCTION ||
	        start.node->type == DOM_COMMENT)) {
		status = range_clone_slice(realm, start.node, start.offset, end.offset, output, pending);
		if (status != 0)
			return status;

		/* Succeeded: this interval needs no child traversal frame. */
		return 0;
	}

	/* Actual parent graphs determine the common ancestor without visible property access. */
	ancestor = range_ancestor(start.node, end.node);
	if (ancestor == NULL)
		return EINVAL;

	/* Preflight every contained doctype before any child clone can change conservative generation. */
	for (child = ancestor->first_child; child != NULL; child = child->next) {
		contained = range_contained(child, start, end);
		if (contained && child->type == DOM_DOCUMENT_TYPE) {
			status = bind_throw_dom(realm, "HierarchyRequestError", "Range contents cannot contain a DocumentType.");
			return status;
		}
	}

	/* The immutable interval traverses direct children in actual tree order. */
	frame->next = ancestor->first_child;

	/* Succeeded: native preflight and the complete ordinary traversal boundary are ready. */
	return 0;
}

/* Copies all partial shells and fully contained subtrees without recursive interval descent. */
static int
range_clone_interval(
	struct vm_realm *realm,
	struct range_point start,
	struct range_point end,
	struct dom_node *output,
	struct vm_cell **pending)
{
	struct wb_vector frames;
	struct range_clone_frame initial;
	struct range_clone_frame child_frame;
	struct range_clone_frame *frame;
	struct range_point child_start;
	struct range_point child_end;
	struct dom_node *child;
	struct dom_node *copy;
	size_t length;
	int contains_start;
	int contains_end;
	int contained;
	int status;

	/* Checked native vector storage owns traversal only; participant graphs have explicit roots. */
	wb_vector_init(&frames, sizeof(struct range_clone_frame));
	status = range_clone_prepare(realm, start, end, output, &initial, pending);
	if (status != 0) {
		wb_vector_release(&frames);
		return status;
	}

	/* Retain the initial traversal in checked native storage. */
	status = wb_vector_push(&frames, &initial);
	if (status != 0) {
		wb_vector_release(&frames);
		return status;
	}

	/* Each unfinished interval resumes its following sibling after a partial child frame retires. */
	while (frames.count != 0) {
		frame = wb_vector_at(&frames, frames.count - 1U);
		child = frame->next;
		if (child == NULL) {
			wb_vector_pop(&frames);
			continue;
		}

		/* Advance the source frame before a push can invalidate its C vector address. */
		frame->next = child->next;
		contains_start = dom_is_inclusive_ancestor(child, frame->start.node);
		contains_end = dom_is_inclusive_ancestor(child, frame->end.node);

		/* Partially contained branches retain a shallow shell even when their selected contents are empty. */
		if (contains_start || contains_end) {
			/* Supported native lengths fit the unsigned interval tuples constructed below. */
			length = range_length(child);
			if (length > UINT32_MAX) {
				wb_vector_release(&frames);
				return EINVAL;
			}

			/* A start branch ends at its complete child boundary unless it also contains the end. */
			child_start.node = child;
			child_start.offset = 0;
			if (contains_start)
				child_start = frame->start;

			/* An end branch begins at zero and ends at the original immutable end tuple. */
			child_end.node = child;
			child_end.offset = (uint32_t)length;
			if (contains_end)
				child_end = frame->end;

			/* Partial character nodes preserve exact slice identity, including zero selected code units. */
			if (child->type == DOM_TEXT ||
			    child->type == DOM_CDATA_SECTION ||
			    child->type == DOM_PROCESSING_INSTRUCTION ||
			    child->type == DOM_COMMENT) {
				status = range_clone_slice(realm, child, child_start.offset, child_end.offset, frame->output, pending);
				if (status != 0) {
					wb_vector_release(&frames);
					return status;
				}

				/* This complete character slice has no descendant frame. */
				continue;
			}

			/* Complete shallow initialization preserves native names, attributes and supported control state. */
			status = bind_clone_node(realm, child, 0, &copy);
			if (status != 0) {
				wb_vector_release(&frames);
				return status;
			}

			/* Output ownership protects the shell through preparation, allocation and subsequent descent. */
			*pending = &copy->cell;
			dom_append_child(frame->output, copy);
			status = range_clone_prepare(realm, child_start, child_end, copy, &child_frame, pending);
			if (status != 0) {
				wb_vector_release(&frames);
				return status;
			}

			/* The next descent can move vector storage, so the old frame is no longer used. */
			status = wb_vector_push(&frames, &child_frame);
			if (status != 0) {
				wb_vector_release(&frames);
				return status;
			}

			/* Resume this source sibling only after the partial child frame finishes. */
			continue;
		}

		/* Fully contained children copy their complete ordinary and supported template graphs. */
		contained = range_contained(child, frame->start, frame->end);
		if (contained) {
			status = bind_clone_node(realm, child, 1, &copy);
			if (status != 0) {
				wb_vector_release(&frames);
				return status;
			}

			/* Retain the complete deep copy before its native link joins the output graph. */
			*pending = &copy->cell;
			dom_append_child(frame->output, copy);
		}
	}

	/* All interval frames are finished; only the complete rooted output graph remains. */
	wb_vector_release(&frames);

	/* Succeeded: native contents are copied in ordinary tree order with exact partial shells. */
	return 0;
}

/* Reads the actual native startContainer through the shared boundary accessor. */
static int
range_startContainer(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Native state branding is independent of mutable wrapper properties. */
	status = range_read(realm, receiver, RANGE_START_CONTAINER, result);
	if (status != 0)
		return status;

	/* Succeeded: the requested native boundary property is available. */
	return 0;
}

/* Reads the actual native endContainer through the shared boundary accessor. */
static int
range_endContainer(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Native state branding is independent of mutable wrapper properties. */
	status = range_read(realm, receiver, RANGE_END_CONTAINER, result);
	if (status != 0)
		return status;

	/* Succeeded: the requested native boundary property is available. */
	return 0;
}

/* Reads the actual native startOffset through the shared boundary accessor. */
static int
range_startOffset(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Native state branding is independent of mutable wrapper properties. */
	status = range_read(realm, receiver, RANGE_START_OFFSET, result);
	if (status != 0)
		return status;

	/* Succeeded: the requested native boundary property is available. */
	return 0;
}

/* Reads the actual native endOffset through the shared boundary accessor. */
static int
range_endOffset(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Native state branding is independent of mutable wrapper properties. */
	status = range_read(realm, receiver, RANGE_END_OFFSET, result);
	if (status != 0)
		return status;

	/* Succeeded: the requested native boundary property is available. */
	return 0;
}

/* Reads the actual native collapsed through the shared boundary accessor. */
static int
range_collapsed(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Native state branding is independent of mutable wrapper properties. */
	status = range_read(realm, receiver, RANGE_COLLAPSED, result);
	if (status != 0)
		return status;

	/* Succeeded: the requested native boundary property is available. */
	return 0;
}

/* Reads the actual native commonAncestorContainer through the shared boundary accessor. */
static int
range_commonAncestorContainer(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Native state branding is independent of mutable wrapper properties. */
	status = range_read(realm, receiver, RANGE_ANCESTOR, result);
	if (status != 0)
		return status;

	/* Succeeded: the requested native boundary property is available. */
	return 0;
}

/* Applies setStart using current native node identity, ancestry and offsets. */
static int
range_setStart(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The shared path validates typed arguments and retains conversion participants. */
	status = range_change(realm, receiver, args, count, RANGE_START, result);
	if (status != 0)
		return status;

	/* Succeeded: the requested boundary operation has updated genuine native state. */
	return 0;
}

/* Applies setEnd using current native node identity, ancestry and offsets. */
static int
range_setEnd(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The shared path validates typed arguments and retains conversion participants. */
	status = range_change(realm, receiver, args, count, RANGE_END, result);
	if (status != 0)
		return status;

	/* Succeeded: the requested boundary operation has updated genuine native state. */
	return 0;
}

/* Applies setStartBefore using current native node identity, ancestry and offsets. */
static int
range_setStartBefore(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The shared path validates typed arguments and retains conversion participants. */
	status = range_change(realm, receiver, args, count, RANGE_START_BEFORE, result);
	if (status != 0)
		return status;

	/* Succeeded: the requested boundary operation has updated genuine native state. */
	return 0;
}

/* Applies setStartAfter using current native node identity, ancestry and offsets. */
static int
range_setStartAfter(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The shared path validates typed arguments and retains conversion participants. */
	status = range_change(realm, receiver, args, count, RANGE_START_AFTER, result);
	if (status != 0)
		return status;

	/* Succeeded: the requested boundary operation has updated genuine native state. */
	return 0;
}

/* Applies setEndBefore using current native node identity, ancestry and offsets. */
static int
range_setEndBefore(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The shared path validates typed arguments and retains conversion participants. */
	status = range_change(realm, receiver, args, count, RANGE_END_BEFORE, result);
	if (status != 0)
		return status;

	/* Succeeded: the requested boundary operation has updated genuine native state. */
	return 0;
}

/* Applies setEndAfter using current native node identity, ancestry and offsets. */
static int
range_setEndAfter(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The shared path validates typed arguments and retains conversion participants. */
	status = range_change(realm, receiver, args, count, RANGE_END_AFTER, result);
	if (status != 0)
		return status;

	/* Succeeded: the requested boundary operation has updated genuine native state. */
	return 0;
}

/* Applies selectNode using current native node identity, ancestry and offsets. */
static int
range_selectNode(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The shared path validates typed arguments and retains conversion participants. */
	status = range_change(realm, receiver, args, count, RANGE_SELECT_NODE, result);
	if (status != 0)
		return status;

	/* Succeeded: the requested boundary operation has updated genuine native state. */
	return 0;
}

/* Applies selectNodeContents using current native node identity, ancestry and offsets. */
static int
range_selectNodeContents(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The shared path validates typed arguments and retains conversion participants. */
	status = range_change(realm, receiver, args, count, RANGE_SELECT_CONTENTS, result);
	if (status != 0)
		return status;

	/* Succeeded: the requested boundary operation has updated genuine native state. */
	return 0;
}

/* Retains creator and current endpoint Documents through the actual native node graph. */
static void
range_trace(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct range_state *state;

	/* Each native endpoint can belong to an owner distinct from the range's original creator. */
	state = (struct range_state *)cell;
	vm_heap_mark(heap, &state->document->node.cell);
	vm_heap_mark(heap, &state->start.node->cell);
	vm_heap_mark(heap, &state->end.node->cell);
}

/* Publishes a complete native range through its actual Document's relevant interface prototype. */
static int
range_create(
	struct vm_realm *realm,
	struct dom_document *document,
	vm_value *result)
{
	struct bind_window *window;
	struct vm_object *prototype;
	struct vm_object *wrapper;
	struct range_state *state;
	struct vm_cell *roots[2];
	vm_value snapshot;
	int status;

	/* A foreign heap cannot share traced native cells with this embedding. */
	if (document->heap != realm->heap)
		return EINVAL;

	/* Root the creator and the pending state before either native allocation. */
	roots[0] = &document->node.cell;
	roots[1] = NULL;
	status = vm_heap_add_root(realm->heap, &roots[0]);
	if (status != 0)
		return status;

	/* Unwind the creator slot if registration of the pending state fails. */
	status = vm_heap_add_root(realm->heap, &roots[1]);
	if (status != 0) {
		vm_heap_remove_root(realm->heap, &roots[0]);
		return status;
	}

	/* XML factory snapshots remain independent of any current Window view. */
	if (document->binding_prototypes != NULL) {
		status = vm_object_get(document->binding_prototypes, vm_value_int32(BIND_RANGE), &snapshot);
		if (status != 0) {
			range_unroot(realm->heap, roots, 2);
			return status;
		}

		/* Missing prototypes cannot publish a partly initialized native factory result. */
		if (snapshot == VM_VALUE_UNDEFINED) {
			range_unroot(realm->heap, roots, 2);
			return EINVAL;
		}

		/* The actual XML factory snapshot owns this concrete native prototype. */
		prototype = (struct vm_object *)vm_value_as_cell(snapshot);
	} else {
		/* Borrowed factories use the receiver's actual owner before the caller fallback. */
		window = document->view;

		/* A receiver without a view uses the invoking binding owner as its fallback. */
		if (window == NULL)
			window = bind_window_of(realm);

		/* An embedding without a binding owner cannot provide native interface methods. */
		if (window == NULL) {
			range_unroot(realm->heap, roots, 2);
			return EINVAL;
		}

		/* The receiver Document chooses the concrete interface from its actual Window. */
		prototype = window->prototypes[BIND_RANGE];
	}

	/* The initialized state becomes rooted before the wrapper allocation can collect. */
	state = vm_heap_alloc(document->heap, &range_type, sizeof(*state));
	if (state == NULL) {
		range_unroot(realm->heap, roots, 2);
		return ENOMEM;
	}

	/* Initialize both boundaries before publishing this traced state. */
	state->document = document;
	state->start.node = &document->node;
	state->start.offset = 0;
	state->end = state->start;
	state->subscription = NULL;
	roots[1] = &state->cell;

	/* Register only a weak removal edge after the rooted native state is fully initialized. */
	status = range_commit(state, state->start, state->end);
	if (status != 0) {
		range_unroot(realm->heap, roots, 2);
		return status;
	}

	/* Only a complete native graph is published as a branded platform object. */
	wrapper = vm_object_create(document->heap, prototype);
	if (wrapper == NULL) {
		range_unroot(realm->heap, roots, 2);
		return ENOMEM;
	}

	/* Publish the complete state with a private native platform brand. */
	wrapper->kind = VM_KIND_PLATFORM;
	wrapper->internal = vm_value_cell(state);
	*result = vm_value_cell(wrapper);
	range_unroot(realm->heap, roots, 2);

	/* Succeeded: the result wrapper now owns its native creator and endpoint graph. */
	return 0;
}

/* Constructs a range at the current global object's actual associated Document. */
static int
range_construct(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	int status;

	UNUSED_PARAMETER(receiver);
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The constructor ignores extra arguments and obtains only the actual current binding owner. */
	window = bind_window_of(realm);
	if (window == NULL)
		return EINVAL;

	/* Publish the complete independent range at the associated Document start. */
	status = range_create(realm, window->document, result);
	if (status != 0)
		return status;

	/* Succeeded: new Range() is collapsed at its current Window's Document start. */
	return 0;
}

/* Rejects prototype lookalikes and platform objects whose native cell is not a range. */
static int
range_this(
	struct vm_realm *realm,
	vm_value receiver,
	struct range_state **out)
{
	struct vm_object *object;
	struct vm_cell *cell;
	int actual;
	int status;

	/* Only a real platform object can carry a private range state. */
	actual = vm_value_is_object(receiver);
	if (!actual) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Inspect actual platform storage independently of the visible prototype chain. */
	object = (struct vm_object *)vm_value_as_cell(receiver);
	actual = vm_value_is_cell(object->internal);
	if (object->kind != VM_KIND_PLATFORM || !actual) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* A genuine Node or iterator cannot borrow native range accessors. */
	cell = vm_value_as_cell(object->internal);
	if (cell->type != &range_type) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Succeeded: the caller may operate on this genuine private native boundary state. */
	*out = (struct range_state *)cell;
	return 0;
}

/* Reads a genuine native boundary field while wrapping actual current node owners. */
static int
range_read(
	struct vm_realm *realm,
	vm_value receiver,
	enum range_field field,
	vm_value *result)
{
	struct range_state *state;
	struct dom_node *node;
	struct bind_window *window;
	int collapsed;
	int status;

	/* Native brands precede current endpoint and common-ancestor observations. */
	status = range_this(realm, receiver, &state);
	if (status != 0)
		return status;

	/* Unsigned offsets use exact Number values even above the signed-int32 boundary. */
	if (field == RANGE_START_OFFSET || field == RANGE_END_OFFSET) {
		/* Choose the requested native unsigned offset without consulting wrapper properties. */
		if (field == RANGE_START_OFFSET) {
			*result = vm_value_number((double)state->start.offset);
		} else {
			*result = vm_value_number((double)state->end.offset);
		}

		/* Succeeded: the actual unsigned endpoint offset has no signed alias. */
		return 0;
	}

	/* Collapsed depends on both actual native containers and offsets. */
	if (field == RANGE_COLLAPSED) {
		collapsed = 0;
		if (state->start.node == state->end.node && state->start.offset == state->end.offset)
			collapsed = 1;

		/* Publish the callback-free native equality observation. */
		*result = vm_value_boolean(collapsed);

		/* Succeeded: script-visible endpoint expandos have no effect on native equality. */
		return 0;
	}

	/* Node wrappers resolve their actual Document view or private XML snapshot. */
	node = state->start.node;
	if (field == RANGE_END_CONTAINER)
		node = state->end.node;

	/* The common container follows the deepest actual shared ancestry. */
	if (field == RANGE_ANCESTOR)
		node = range_ancestor(state->start.node, state->end.node);

	/* Wrap the actual current node owner using the invoking binding as its fallback. */
	window = bind_window_of(realm);
	status = bind_wrap_or_null(window, node, result);
	if (status != 0)
		return status;

	/* Succeeded: actual container identity is preserved across borrowed getter calls. */
	return 0;
}

/* Applies typed boundary methods while protecting every participant through offset conversion. */
static int
range_change(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	enum range_action action,
	vm_value *result)
{
	struct range_state *state;
	struct dom_node *node;
	struct vm_cell *roots[3];
	vm_value offset;
	unsigned required;
	unsigned index;
	int cell;
	int status;

	/* Native receiver branding precedes required argument and WebIDL conversions. */
	*result = VM_VALUE_UNDEFINED;
	status = range_this(realm, receiver, &state);
	if (status != 0)
		return status;

	/* Explicit endpoint setters require an offset in addition to the typed Node. */
	required = 1;
	if (action == RANGE_START || action == RANGE_END)
		required = 2;

	/* Missing required arguments reject before observable numeric conversion. */
	if (count < required) {
		status = vm_throw_type_error(realm, "The range operation is missing a required argument.");
		return status;
	}

	/* The first typed Node is established before numeric conversion of the second argument. */
	status = bind_argument_node(realm, args[0], &node);
	if (status != 0)
		return status;

	/* Cells from another collector cannot participate in this native boundary graph. */
	if (node->document->heap != realm->heap)
		return EINVAL;

	/* Retain genuine native state and every cell-valued participant in conversion. */
	offset = js_argument(args, count, 1);
	roots[0] = &state->cell;
	roots[1] = &node->cell;
	roots[2] = NULL;
	cell = vm_value_is_cell(offset);
	if (cell)
		roots[2] = vm_value_as_cell(offset);

	/* Reentrant conversion may detach endpoints, target and argument from all other roots. */
	for (index = 0; index < 3U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0) {
			range_unroot(realm->heap, roots, index);
			return status;
		}
	}

	/* Resolve actual current node length, parent and ancestry only after conversion. */
	status = range_execute(realm, state, node, args, count, action);
	range_unroot(realm->heap, roots, 3);
	if (status != 0)
		return status;

	/* Succeeded: native endpoints are updated and no temporary root slot remains registered. */
	return 0;
}

/* Resolves one operation's current target container and offset without consulting script fields. */
static int
range_execute(
	struct vm_realm *realm,
	struct range_state *state,
	struct dom_node *node,
	const vm_value *args,
	unsigned count,
	enum range_action action)
{
	struct dom_node *parent;
	struct range_point start;
	struct range_point end;
	size_t length;
	size_t position;
	uint32_t offset;
	int status;

	/* Explicit endpoint offsets undergo exactly one WebIDL unsigned-long conversion. */
	if (action == RANGE_START || action == RANGE_END) {
		status = vm_to_uint32(realm, js_argument(args, count, 1), &offset);
		if (status != 0)
			return status;

		/* Validate the converted point against current native type, length and ordering. */
		status = range_apply(realm, state, node, offset, action);
		if (status != 0)
			return status;

		/* Succeeded: current converted boundary ordering is reflected in native state. */
		return 0;
	}

	/* Contents selection rejects doctypes before reading their otherwise empty native length. */
	if (action == RANGE_SELECT_CONTENTS) {
		if (node->type == DOM_DOCUMENT_TYPE) {
			status = bind_throw_dom(realm, "InvalidNodeTypeError", "A doctype cannot contain range boundaries.");
			return status;
		}

		/* Character data uses UTF16 units; ordinary containers use actual current children. */
		length = range_length(node);
		if (length > UINT32_MAX)
			return EOVERFLOW;

		/* Commit the complete current contents interval only after validating its native length. */
		start.node = node;
		start.offset = 0;
		end.node = node;
		end.offset = (uint32_t)length;
		status = range_commit(state, start, end);
		if (status != 0)
			return status;

		/* Succeeded: the complete current node contents supply both native boundaries. */
		return 0;
	}

	/* Relative boundaries and selectNode require an actual current parent. */
	parent = node->parent;
	if (parent == NULL) {
		status = bind_throw_dom(realm, "InvalidNodeTypeError", "A parentless node has no surrounding boundary.");
		return status;
	}

	/* Resolve the current surrounding child interval before mutating either endpoint. */
	position = range_index(node);
	if (position >= UINT32_MAX)
		return EOVERFLOW;
	offset = (uint32_t)position;

	/* Selecting a node preserves its parent's complete one-child interval. */
	if (action == RANGE_SELECT_NODE) {
		start.node = parent;
		start.offset = offset;
		end.node = parent;
		end.offset = offset + 1U;
		status = range_commit(state, start, end);
		if (status != 0)
			return status;

		/* Succeeded: the native selection surrounds exactly this actual current child. */
		return 0;
	}

	/* After-boundaries count the supplied node itself; before-boundaries do not. */
	if (action == RANGE_START_AFTER || action == RANGE_END_AFTER)
		offset++;

	/* Choose which endpoint this relative boundary operation updates. */
	if (action == RANGE_START_BEFORE || action == RANGE_START_AFTER) {
		action = RANGE_START;
	} else {
		action = RANGE_END;
	}

	/* Validate and order the actual parent boundary before its native commit. */
	status = range_apply(realm, state, parent, offset, action);
	if (status != 0)
		return status;

	/* Succeeded: actual parent/index ancestry determines the relative native boundary. */
	return 0;
}

/* Validates a current native point before preserving or collapsing the opposite endpoint. */
static int
range_apply(
	struct vm_realm *realm,
	struct range_state *state,
	struct dom_node *node,
	uint32_t offset,
	enum range_action action)
{
	struct range_point point;
	struct range_point other;
	struct range_point start;
	struct range_point end;
	struct dom_node *root;
	struct dom_node *other_root;
	size_t length;
	int collapse;
	int comparison;
	int status;

	/* Actual invalid container type precedes native index validation. */
	if (node->type == DOM_DOCUMENT_TYPE) {
		status = bind_throw_dom(realm, "InvalidNodeTypeError", "A doctype cannot contain range boundaries.");
		return status;
	}

	/* Validate the requested offset against the current native container length. */
	length = range_length(node);
	if ((size_t)offset > length) {
		status = bind_throw_dom(realm, "IndexSizeError", "The range offset exceeds the current node length.");
		return status;
	}

	/* Prepare the requested native tuple and identify the opposite endpoint. */
	point.node = node;
	point.offset = offset;
	other = state->start;
	if (action == RANGE_START)
		other = state->end;

	/* Different current tree roots collapse the opposite endpoint regardless of creation Document. */
	root = range_root(node);
	other_root = range_root(other.node);
	collapse = 0;
	if (root != other_root) {
		collapse = 1;
	} else {
		comparison = range_compare(point, other);
		if (action == RANGE_START && comparison > 0)
			collapse = 1;

		/* An end before its existing start collapses the start to that requested end. */
		if (action == RANGE_END && comparison < 0)
			collapse = 1;
	}

	/* Prepare both prospective tuples before any checked subscription replacement. */
	start = state->start;
	end = state->end;
	if (action == RANGE_START) {
		/* A crossed or disconnected start replaces the opposite endpoint. */
		if (collapse)
			end = point;

		/* The requested start defines the prospective current owner. */
		start = point;
	} else {
		/* A crossed or disconnected end replaces the opposite endpoint. */
		if (collapse)
			start = point;

		/* The requested end preserves the start's owner unless both points collapse. */
		end = point;
	}

	/* Subscription preparation can fail without exposing either prospective boundary. */
	status = range_commit(state, start, end);
	if (status != 0)
		return status;

	/* Succeeded: both native endpoint fields preserve the requested current boundary relation. */
	return 0;
}

/* Collapses to the actual start when true, otherwise to the actual end without numeric conversion. */
static int
range_collapse(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct range_state *state;
	struct range_point point;
	int start;
	int status;

	/* Native receiver validation precedes the callback-free optional Boolean conversion. */
	*result = VM_VALUE_UNDEFINED;
	status = range_this(realm, receiver, &state);
	if (status != 0)
		return status;

	/* Optional Boolean conversion chooses the original start or the default end without user callbacks. */
	start = vm_to_boolean(js_argument(args, count, 0));
	if (start) {
		point = state->start;
	} else {
		point = state->end;
	}

	/* Reassociate the current start container without changing the original creator. */
	status = range_commit(state, point, point);
	if (status != 0)
		return status;

	/* Succeeded: both endpoint tuples now equal the selected original native tuple. */
	return 0;
}

/* Preserves legacy detach as a branded no-op without invalidating native boundaries. */
static int
range_detach(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct range_state *state;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Detach has no active/inactive lifecycle flag and does not release the endpoint graph. */
	status = range_this(realm, receiver, &state);
	if (status != 0)
		return status;

	/* Succeeded: this genuine range remains usable after the compatibility no-op. */
	*result = VM_VALUE_UNDEFINED;
	return 0;
}

/* Clones genuine native points into a separately owned live state in the source's relevant realm. */
static int
range_clone(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct range_state *source;
	struct range_state *copy;
	struct vm_object *wrapper;
	struct vm_cell *roots[2];
	vm_value cloned;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Source creator and native fields remain authoritative after wrapper prototype pollution. */
	*result = VM_VALUE_UNDEFINED;
	status = range_this(realm, receiver, &source);
	if (status != 0)
		return status;

	/* Protect source points and pending publication through real factory allocation and collection. */
	roots[0] = &source->cell;
	roots[1] = NULL;
	status = vm_heap_add_root(realm->heap, &roots[0]);
	if (status != 0)
		return status;

	/* The second slot keeps a complete pending wrapper separate from the public result. */
	status = vm_heap_add_root(realm->heap, &roots[1]);
	if (status != 0) {
		range_unroot(realm->heap, roots, 1);
		return status;
	}

	/* Ordinary and borrowed calls both choose the source's original Document/XML prototype owner. */
	cloned = VM_VALUE_UNDEFINED;
	status = range_create(realm, source->document, &cloned);
	if (status != 0) {
		range_unroot(realm->heap, roots, 2);
		return status;
	}

	/* The new wrapper privately owns a distinct initialized native state and weak token. */
	roots[1] = vm_value_as_cell(cloned);
	wrapper = (struct vm_object *)roots[1];
	copy = (struct range_state *)vm_value_as_cell(wrapper->internal);
	status = range_commit(copy, source->start, source->end);
	if (status != 0) {
		range_unroot(realm->heap, roots, 2);
		return status;
	}

	/* Publish only the complete independently live clone after its current-owner subscription agrees. */
	*result = cloned;
	range_unroot(realm->heap, roots, 2);

	/* Succeeded: future setters and removals can update either native range independently. */
	return 0;
}

/* Converts numeric mode before the typed source, then compares actual current native points without mutation. */
static int
range_compare_boundaries(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct range_state *state;
	struct range_state *source;
	struct range_point point;
	struct range_point other;
	struct dom_node *root;
	struct dom_node *source_root;
	struct vm_cell *roots[3];
	uint32_t converted;
	unsigned how;
	unsigned index;
	int cell;
	int comparison;
	int status;

	/* Genuine receiver identity precedes all required argument and mode conversions. */
	*result = VM_VALUE_UNDEFINED;
	status = range_this(realm, receiver, &state);
	if (status != 0)
		return status;

	/* Both mode and source are required even when an omitted value could numerically become zero. */
	if (count < 2U) {
		status = vm_throw_type_error(realm, "Boundary comparison requires a mode and a Range.");
		return status;
	}

	/* The source wrapper may lose all other references during first-argument numeric conversion. */
	roots[0] = &state->cell;
	roots[1] = NULL;
	roots[2] = NULL;
	cell = vm_value_is_cell(args[0]);
	if (cell)
		roots[1] = vm_value_as_cell(args[0]);

	/* Retain a genuine source or an unconverted cell before its later typed-interface conversion. */
	cell = vm_value_is_cell(args[1]);
	if (cell)
		roots[2] = vm_value_as_cell(args[1]);

	/* Every partially registered slot unwinds before an allocation failure returns. */
	for (index = 0; index < 3U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0) {
			range_unroot(realm->heap, roots, index);
			return status;
		}
	}

	/* Low sixteen bits implement unsigned-short modulo after exactly one ToNumber conversion. */
	status = vm_to_uint32(realm, args[0], &converted);
	if (status != 0) {
		range_unroot(realm->heap, roots, 3);
		return status;
	}

	/* Typed source conversion follows the first numeric conversion and precedes algorithm errors. */
	how = converted & 0xFFFFU;
	status = range_this(realm, args[1], &source);
	if (status != 0) {
		range_unroot(realm->heap, roots, 3);
		return status;
	}

	/* Unsupported modes reject before checking whether the actual current ordinary roots differ. */
	if (how > 3U) {
		status = bind_throw_dom(realm, "NotSupportedError", "The range comparison mode is unsupported.");
		range_unroot(realm->heap, roots, 3);
		return status;
	}

	/* Conversion callbacks may have changed either range's points or their actual current roots. */
	root = range_root(state->start.node);
	source_root = range_root(source->start.node);
	if (root != source_root) {
		status = bind_throw_dom(realm, "WrongDocumentError", "The ranges have different ordinary roots.");
		range_unroot(realm->heap, roots, 3);
		return status;
	}

	/* Historical mode names order this range's tuple against the source range's tuple. */
	point = state->start;
	other = source->start;
	if (how == 1U || how == 2U)
		point = state->end;

	/* END_TO_END and END_TO_START both select the source's end tuple. */
	if (how == 2U || how == 3U)
		other = source->end;

	/* Native comparison observes current ancestry and unsigned offsets without invoking script. */
	comparison = range_compare(point, other);
	*result = vm_value_int32(comparison);
	range_unroot(realm->heap, roots, 3);

	/* Succeeded: negative, equal or positive order was reported without changing either range. */
	return 0;
}

/* Copies selected actual Text units before publishing a new independently owned VM string. */
static int
range_stringify(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct range_state *state;
	struct dom_node *root;
	struct dom_node *node;
	struct vm_cell *state_root;
	struct wb_units units;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Native receiver branding excludes prototype lookalikes and visible boundary expandos. */
	status = range_this(realm, receiver, &state);
	if (status != 0)
		return status;

	/* Explicit state ownership also protects native creator/endpoints during final string allocation. */
	state_root = &state->cell;
	status = vm_heap_add_root(realm->heap, &state_root);
	if (status != 0)
		return status;

	/* Ordinary native preorder excludes separate template content and has no depth truncation. */
	wb_units_init(&units);
	root = range_root(state->start.node);
	node = root;
	while (node != NULL) {
		/* Only Text contributes data; comments, tags and layout have no string content here. */
		if (node->type == DOM_TEXT || node->type == DOM_CDATA_SECTION) {
			status = range_append_text(state, (struct dom_character_data *)node, &units);
			if (status != 0) {
				wb_units_release(&units);
				vm_heap_remove_root(realm->heap, &state_root);
				return status;
			}
		}

		/* Native links determine the next actual node without visible childNodes properties. */
		node = range_next_node(root, node);
	}

	/* The copied independent units remain valid even if VM allocation collects unrelated native graphs. */
	status = bind_units(realm, units.data, units.length, result);
	wb_units_release(&units);
	vm_heap_remove_root(realm->heap, &state_root);
	if (status != 0)
		return status;

	/* Succeeded: selected native UTF16 text was published without changing the original range. */
	return 0;
}

/* Appends one full or partial actual Text node while excluding text outside the native interval. */
static int
range_append_text(
	struct range_state *state,
	struct dom_character_data *text,
	struct wb_units *units)
{
	struct range_point point;
	struct dom_node *node;
	size_t begin;
	size_t end;
	size_t count;
	int comparison;
	int status;

	/* This finite foundation uses uint32-representable native points for Text containment. */
	node = &text->node;
	if (text->data.length > UINT32_MAX)
		return EOVERFLOW;

	/* Start and end Text nodes contribute exactly their selected UTF16 suffix, prefix or same-node slice. */
	begin = 0;
	end = text->data.length;
	if (node == state->start.node)
		begin = state->start.offset;

	/* Equal start/end Text containers apply both limits without duplicating their data. */
	if (node == state->end.node)
		end = state->end.offset;

	/* Other Text nodes contribute only when both of their actual native endpoints lie inside the range. */
	if (node != state->start.node && node != state->end.node) {
		point.node = node;
		point.offset = 0;
		comparison = range_compare(point, state->start);
		if (comparison <= 0)
			return 0;

		/* The complete Text interval must also precede the requested native end point. */
		point.offset = (uint32_t)text->data.length;
		comparison = range_compare(point, state->end);
		if (comparison >= 0)
			return 0;
	}

	/* Unsupported stale CharacterData mutation cannot turn invalid native offsets into an out-of-bounds read. */
	if (begin > text->data.length ||
	    end > text->data.length ||
	    begin > end)
		return EINVAL;

	/* Empty intervals avoid pointer arithmetic on an empty native buffer. */
	count = end - begin;
	if (count == 0)
		return 0;

	/* UTF16 concatenation must fit both the native unit count and its byte allocation arithmetic. */
	if (count > SIZE_MAX - units->length)
		return EOVERFLOW;

	/* The buffer's count cannot overflow when converted into bytes for reserve and copy. */
	if (units->length + count > SIZE_MAX / sizeof(uint16_t))
		return EOVERFLOW;

	/* Copy only actual native data after validating the complete selected interval. */
	status = wb_units_append(units, text->data.data + begin, count);
	if (status != 0)
		return status;

	/* Succeeded: this selected Text contributes its exact UTF16 units in tree order. */
	return 0;
}

/* Advances ordinary preorder within one actual root without crossing separate fragments. */
static struct dom_node *
range_next_node(
	struct dom_node *root,
	struct dom_node *node)
{
	/* Descendants precede every following sibling in ordinary native tree order. */
	if (node->first_child != NULL)
		return node->first_child;

	/* Ascend until a following sibling exists inside this actual ordinary root. */
	while (node != root) {
		/* A following sibling is the next node after this complete subtree. */
		if (node->next != NULL)
			return node->next;

		/* The next ancestor may have the next following sibling. */
		node = node->parent;
	}

	/* Succeeded: every actual node within the root has been visited. */
	return NULL;
}

/* Releases only the independently owned C token without touching a possibly finalized Document. */
static void
range_finalize(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct range_state *state;

	UNUSED_PARAMETER(heap);

	/* Unpublished allocation failures and fully constructed ranges share exact token cleanup. */
	state = (struct range_state *)cell;
	dom_removal_unsubscribe(state->subscription);
	state->subscription = NULL;

	/* Succeeded: no weak Document callback can reach this reclaimed native state. */
	return;
}

/* Inserts a typed native Node while retaining state and actual mutation participants through host GC. */
static int
range_insert(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct range_state *state;
	struct dom_node *node;
	struct vm_cell *roots[5];
	unsigned index;
	int status;

	/* Genuine Range branding precedes required arguments and typed Node conversion. */
	*result = VM_VALUE_UNDEFINED;
	status = range_this(realm, receiver, &state);
	if (status != 0)
		return status;

	/* A missing incoming Node cannot enter the native insertion algorithm. */
	if (count < 1U) {
		status = vm_throw_type_error(realm, "Range.insertNode requires a Node.");
		return status;
	}

	/* Convert the required native Node before starting the insertion algorithm. */
	status = bind_argument_node(realm, args[0], &node);
	if (status != 0)
		return status;

	/* Incoming native cells must belong to this collector. */
	if (node->document->heap != realm->heap)
		return EINVAL;

	/* Native state and snapshots remain owned even after split or removal changes the current endpoints. */
	roots[0] = &state->cell;
	roots[1] = &node->cell;
	roots[2] = &state->start.node->cell;
	roots[3] = NULL;
	roots[4] = NULL;
	for (index = 0; index < 5U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0) {
			range_unroot(realm->heap, roots, index);
			return status;
		}
	}

	/* Shared actual DOM mutation hooks may collect without an outer VM receiver call frame. */
	status = range_insert_native(realm, state, node, roots);
	range_unroot(realm->heap, roots, 5);
	if (status != 0)
		return status;

	/* Succeeded: the actual node or Fragment was inserted and current native collapse was reconciled. */
	return 0;
}

/* Performs validated insertion in the actual start container, never through script-visible point fields. */
static int
range_insert_native(
	struct vm_realm *realm,
	struct range_state *state,
	struct dom_node *node,
	struct vm_cell **roots)
{
	struct dom_node *container;
	struct dom_node *reference;
	struct dom_node *parent;
	struct range_point end;
	uint32_t index;
	size_t position;
	size_t count;
	int status;

	/* Comment starts, parentless Text and self-container insertion cannot identify an admissible parent. */
	container = state->start.node;
	if (container->type == DOM_COMMENT ||
	    container->type == DOM_PROCESSING_INSTRUCTION ||
	    ((container->type == DOM_TEXT ||
	      container->type == DOM_CDATA_SECTION) &&
	     container->parent == NULL) ||
	    container == node) {
		status = bind_throw_dom(realm, "HierarchyRequestError", "The Range start cannot receive this node.");
		return status;
	}

	/* Text splits at its own reference; other containers use their actual child boundary. */
	if (container->type == DOM_TEXT || container->type == DOM_CDATA_SECTION) {
		reference = container;
	} else {
		/* Only native ordinary child links select the reference at the current offset. */
		reference = container->first_child;
		index = 0;
		while (reference != NULL && index < state->start.offset) {
			reference = reference->next;
			index++;
		}
	}

	/* The reference determines an actual parent that survives later removal and callbacks. */
	parent = container;
	if (reference != NULL) {
		parent = reference->parent;
		roots[3] = &reference->cell;
	}

	/* The actual parent remains alive even if split or removal changes all current endpoint containers. */
	roots[4] = &parent->cell;

	/* Whole pre-insertion validity must succeed before the original Text can be split. */
	status = bind_validate_insert(realm, parent, node, reference);
	if (status != 0)
		return status;

	/* The shared rooted helper returns the actual suffix and repairs every existing live Range. */
	if (container->type == DOM_TEXT || container->type == DOM_CDATA_SECTION) {
		status = bind_split_text(realm, container, state->start.offset, &reference);
		if (status != 0)
			return status;
		roots[3] = &reference->cell;
	}

	/* Moving a node before itself uses its next sibling before original removal occurs. */
	if (reference == node) {
		reference = node->next;
		roots[3] = NULL;
		if (reference != NULL)
			roots[3] = &reference->cell;
	}

	/* Checked adoption prepares destination ownership before original removal, then detaches exactly once. */
	status = dom_adopt(parent->document, node);
	if (status != 0)
		return status;

	/* Native post-removal links supply the final inserted interval's end position. */
	if (reference == NULL) {
		position = range_length(parent);
	} else {
		/* A callback that invalidated the reference cannot supply an index in this parent. */
		if (reference->parent != parent) {
			status = bind_throw_dom(realm, "NotFoundError", "The Range insertion reference left its parent.");
			return status;
		}

		/* Count the reference only after its actual parent has been verified. */
		position = range_index(reference);
	}

	/* A Fragment contributes its current children rather than one container node. */
	count = 1;
	if (node->type == DOM_DOCUMENT_FRAGMENT)
		count = range_length(node);

	/* This finite native implementation accepts only representable resulting child boundaries. */
	if (position > UINT32_MAX || count > UINT32_MAX - position)
		return EINVAL;
	end.node = parent;
	end.offset = (uint32_t)(position + count);

	/* Ordinary insertion supplies destination repair, checked environment work and actual owner host hooks. */
	status = bind_insert(realm, parent, node, reference);
	if (status != 0)
		return status;

	/* Collapse is tested after actual split, original removal and insertion have repaired this state. */
	if (state->start.node == state->end.node && state->start.offset == state->end.offset) {
		status = range_commit(state, state->start, end);
		if (status != 0)
			return status;
	}

	/* Succeeded: the native Range describes the actual inserted interval when formerly collapsed. */
	return 0;
}

/* Prepares the actual current Document subscription before publishing either prospective boundary. */
static int
range_commit(
	struct range_state *state,
	struct range_point start,
	struct range_point end)
{
	struct dom_removal_subscription *subscription;
	struct dom_removal_subscription *previous;
	struct dom_document *document;
	int matches;
	int status;

	/* Actual current endpoint ownership determines removal delivery independently of creation. */
	document = start.node->document;
	subscription = state->subscription;
	matches = dom_removal_matches_document(subscription, document);
	if (!matches) {
		status = dom_removal_subscribe(document, state, range_removed, &subscription);
		if (status != 0)
			return status;

		/* Optional pure repairs follow this same token during checked adoption and finalization. */
		dom_removal_set_updates(subscription, range_inserted, range_data_changed);
		dom_removal_set_split(subscription, range_text_split);
	}

	/* No script, collector or DOM notification can run between preparation and this native commit. */
	previous = state->subscription;
	state->start = start;
	state->end = end;
	state->subscription = subscription;
	dom_removal_rebind_root(subscription, start.node);

	/* The old weak token is released only after the new native graph and association are complete. */
	if (!matches)
		dom_removal_unsubscribe(previous);

	/* Succeeded: both current endpoints and their one weak removal subscription agree. */
	return 0;
}

/* Repairs both endpoints while the original parent, ancestry and sibling links remain intact. */
static void
range_removed(
	void *context,
	struct dom_node *node)
{
	struct range_state *state;
	struct dom_node *parent;
	size_t index;

	/* Parentless operations supply no surrounding boundary and perform no mutation. */
	parent = node->parent;
	if (parent == NULL)
		return;

	/* This finite implementation accepts only native uint32-representable surrounding points. */
	index = range_index(node);
	if (index > UINT32_MAX)
		return;

	/* Inclusive removed descendants relocate before parent offsets are decremented. */
	state = context;
	range_repair_point(&state->start, node, parent, (uint32_t)index);
	range_repair_point(&state->end, node, parent, (uint32_t)index);

	/* Only the weak associated node changes during notification; no list or owner count changes. */
	dom_removal_rebind_root(state->subscription, state->start.node);

	/* Succeeded: checked adoption now sees the repaired actual start rather than a removed descendant. */
	return;
}

/* Transfers attached split Text points and advances equal surrounding parent boundaries. */
static void
range_text_split(
	void *context,
	struct dom_node *node,
	struct dom_node *created,
	uint32_t offset)
{
	struct range_state *state;
	struct dom_node *parent;
	size_t index;

	/* Detached Text keeps its containers and is repaired only by suffix data truncation. */
	parent = node->parent;
	if (parent == NULL)
		return;

	/* The newly inserted suffix follows the original and identifies its former trailing boundary. */
	index = range_index(created);
	if (index > UINT32_MAX)
		return;

	/* Both native points obey the same special split transfer independently. */
	state = context;
	range_repair_split(&state->start, node, created, parent, offset, (uint32_t)index);
	range_repair_split(&state->end, node, created, parent, offset, (uint32_t)index);
	dom_removal_rebind_root(state->subscription, state->start.node);

	/* Succeeded: later detached adoption follows the actual post-split start container. */
	return;
}

/* Preserves equal Text offsets while moving following units or an equal parent boundary. */
static void
range_repair_split(
	struct range_point *point,
	struct dom_node *node,
	struct dom_node *created,
	struct dom_node *parent,
	uint32_t offset,
	uint32_t index)
{
	/* Only units strictly following the split move into the new same-Document Text. */
	if (point->node == node && point->offset > offset) {
		point->node = created;
		point->offset -= offset;
		return;
	}

	/* Ordinary insertion already repaired greater parent offsets; the equal trailing offset moves once now. */
	if (point->node == parent &&
	    point->offset == index &&
	    point->offset < UINT32_MAX)
		point->offset++;

	/* Succeeded: unrelated, earlier and equal Text boundaries retain their original tuples. */
	return;
}

/* Shifts only surrounding parent boundaries strictly after the actual inserted child. */
static void
range_inserted(
	void *context,
	struct dom_node *node)
{
	struct range_state *state;
	struct dom_node *parent;
	size_t index;

	/* Completed links provide the ordinary insertion position without visible script properties. */
	parent = node->parent;
	if (parent == NULL)
		return;

	/* Count preceding ordinary siblings only after the actual parent is known. */
	index = range_index(node);
	if (index > UINT32_MAX)
		return;

	/* Equal offsets stay before the new child; a following representable start gains one sibling. */
	state = context;
	if (state->start.node == parent &&
	    state->start.offset > index &&
	    state->start.offset < UINT32_MAX)
		state->start.offset++;

	/* The end obeys the same strict comparison independently of the start. */
	if (state->end.node == parent &&
	    state->end.offset > index &&
	    state->end.offset < UINT32_MAX)
		state->end.offset++;

	/* Succeeded: endpoint containers and the token's weak root association remain unchanged. */
	return;
}

/* Repairs both matching CharacterData endpoints after the complete UTF16 replacement. */
static void
range_data_changed(
	void *context,
	struct dom_node *node,
	size_t offset,
	size_t removed,
	size_t added)
{
	struct range_state *state;

	/* Native tuple repair needs neither a wrapper nor allocation or registry mutation. */
	state = context;
	range_repair_data(&state->start, node, offset, removed, added);
	range_repair_data(&state->end, node, offset, removed, added);

	/* Succeeded: both points retain the same actual container and current-owner subscription. */
	return;
}

/* Clamps a point inside the replaced interval or shifts a point following that interval. */
static void
range_repair_data(
	struct range_point *point,
	struct dom_node *node,
	size_t offset,
	size_t removed,
	size_t added)
{
	size_t limit;
	size_t shifted;

	/* Unrelated containers and offsets before or at the replacement retain their boundary. */
	if (point->node != node || point->offset <= offset)
		return;

	/* Invalid embedding lengths cannot overflow the interval comparison. */
	if (removed > SIZE_MAX - offset)
		return;
	limit = offset + removed;

	/* A point inside or at the removed suffix becomes the replacement's start. */
	if (point->offset <= limit) {
		point->offset = (uint32_t)offset;
		return;
	}

	/* A following point gains inserted units after losing the removed units. */
	shifted = point->offset - removed;
	if (added > UINT32_MAX - shifted)
		return;
	point->offset = (uint32_t)(shifted + added);

	/* Succeeded: the updated offset counts the committed native UTF16 buffer. */
	return;
}

/* Relocates one removed inclusive descendant or shifts its parent's following child offset. */
static void
range_repair_point(
	struct range_point *point,
	struct dom_node *node,
	struct dom_node *parent,
	uint32_t index)
{
	struct dom_node *ancestor;

	/* Original ordinary ancestry identifies endpoints inside the subtree being removed. */
	ancestor = point->node;
	while (ancestor != NULL) {
		/* Inclusive descendants become the old parent's position before the removed child. */
		if (ancestor == node) {
			point->node = parent;
			point->offset = index;
			return;
		}

		/* The next original ancestor may be the removed subtree root. */
		ancestor = ancestor->parent;
	}

	/* Offsets after this child lose one preceding sibling; equal or earlier offsets do not. */
	if (point->node == parent && point->offset > index)
		point->offset--;

	/* Succeeded: unrelated containers and offsets keep their original boundary point. */
	return;
}

/* Finds a current ordinary root without crossing a Document or separate template fragment. */
static struct dom_node *
range_root(
	struct dom_node *node)
{
	/* Ordinary parent links determine the current root independently of node.document. */
	while (node->parent != NULL)
		node = node->parent;

	/* Succeeded: this node is the actual current ordinary root. */
	return node;
}

/* Finds the deepest shared current ancestor, including either supplied container itself. */
static struct dom_node *
range_ancestor(
	struct dom_node *left,
	struct dom_node *right)
{
	struct dom_node *candidate;
	struct dom_node *node;

	/* Search nearest left ancestors first, preserving the deepest shared ordinary container. */
	candidate = left;
	while (candidate != NULL) {
		node = right;
		while (node != NULL) {
			/* The first matching left ancestor is the deepest shared container. */
			if (node == candidate)
				return candidate;

			/* Continue toward the right root until this candidate is found. */
			node = node->parent;
		}

		/* The next ancestor may contain the other endpoint's branch. */
		candidate = candidate->parent;
	}

	/* Unsupported intervening mutations may separate roots until a later live-repair increment. */
	return NULL;
}

/* Counts actual UTF16 units or ordinary direct children according to native container type. */
static size_t
range_length(
	struct dom_node *node)
{
	struct dom_character_data *data;
	struct dom_node *child;
	size_t length;

	/* Text and comments expose their actual character buffer rather than script-visible data. */
	if (node->type == DOM_TEXT ||
	    node->type == DOM_CDATA_SECTION ||
	    node->type == DOM_PROCESSING_INSTRUCTION ||
	    node->type == DOM_COMMENT) {
		data = (struct dom_character_data *)node;
		return data->data.length;
	}

	/* Ordinary node length counts current direct children, excluding separate template contents. */
	length = 0;
	child = node->first_child;
	while (child != NULL) {
		length++;
		child = child->next;
	}

	/* Succeeded: current direct child count supplies this container's native boundary limit. */
	return length;
}

/* Counts current preceding siblings without reading visible childNodes or node indices. */
static size_t
range_index(
	struct dom_node *node)
{
	size_t index;

	/* Only actual preceding siblings contribute to the native child position. */
	index = 0;
	node = node->previous;
	while (node != NULL) {
		index++;
		node = node->previous;
	}

	/* Succeeded: this exact child position precedes every following sibling. */
	return index;
}

/* Compares two native boundary points known to share an ordinary tree root. */
static int
range_compare(
	struct range_point left,
	struct range_point right)
{
	struct dom_node *ancestor;
	struct dom_node *left_branch;
	struct dom_node *right_branch;
	struct dom_node *node;
	size_t index;

	/* Offsets in one actual container are compared directly. */
	if (left.node == right.node) {
		/* A lower offset in the same container is the earlier point. */
		if (left.offset < right.offset)
			return -1;

		/* A higher offset in the same container is the later point. */
		if (left.offset > right.offset)
			return 1;

		/* Equal native tuples describe the same boundary point. */
		return 0;
	}

	/* A left ancestor's child boundary orders the entire right descendant branch. */
	node = right.node;
	while (node->parent != NULL) {
		/* The left container encloses this exact right descendant branch. */
		if (node->parent == left.node) {
			index = range_index(node);
			if (index < (size_t)left.offset)
				return 1;

			/* This left boundary lies before the enclosed right branch. */
			return -1;
		}

		/* Continue toward a possible containing left endpoint. */
		node = node->parent;
	}

	/* A right ancestor's child boundary orders the entire left descendant branch symmetrically. */
	node = left.node;
	while (node->parent != NULL) {
		/* The right container encloses this exact left descendant branch. */
		if (node->parent == right.node) {
			index = range_index(node);
			if (index < (size_t)right.offset)
				return -1;

			/* This right boundary lies before the enclosed left branch. */
			return 1;
		}

		/* Continue toward a possible containing right endpoint. */
		node = node->parent;
	}

	/* Disjoint endpoint branches are ordered by their actual common ancestor's child links. */
	ancestor = range_ancestor(left.node, right.node);
	left_branch = left.node;
	while (left_branch->parent != ancestor)
		left_branch = left_branch->parent;

	/* Resolve the distinct right branch beneath the same actual ancestor. */
	right_branch = right.node;
	while (right_branch->parent != ancestor)
		right_branch = right_branch->parent;

	/* A right branch among preceding siblings makes the left boundary later. */
	node = left_branch->previous;
	while (node != NULL) {
		/* A preceding right branch places the left point after the right point. */
		if (node == right_branch)
			return 1;
		node = node->previous;
	}

	/* Succeeded: the left branch occurs before the distinct right branch. */
	return -1;
}

/* Releases exactly the slots registered by one construction or boundary invocation. */
static void
range_unroot(
	struct vm_heap *heap,
	struct vm_cell **roots,
	unsigned count)
{
	unsigned index;

	/* Temporary slots retain nothing after normal, conversion or allocation-error exits. */
	for (index = 0; index < count; index++)
		vm_heap_remove_root(heap, &roots[index]);
}
