/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Copies supported native node graphs with callee roots and checked iterative descent.
 */

#include "bind/internal.h"

#include <errno.h>

/* One unfinished subtree refers only to participants retained by the source and copy roots. */
struct clone_frame {
	struct dom_node *source;
	struct dom_node *copy;
	struct dom_node *destination;
	struct dom_node *next;
	/* Ordinary children are followed once by the source's separate template graph. */
	int content_pending;
};

static int clone_shallow(struct dom_node *source, struct vm_cell **pending, struct dom_node **created);
static int clone_descendants(struct dom_node *source, struct dom_node *copy, struct vm_cell **pending);
static void clone_unroot(struct vm_heap *heap, struct vm_cell **roots, unsigned count);

/*
 * Copies a supported node and optionally its complete ordinary and template subtrees.
 *
 * The caller receives a native node only after every checked allocation succeeds.
 */
int
bind_clone_node(
	struct vm_realm *realm,
	struct dom_node *source,
	int deep,
	struct dom_node **created)
{
	struct dom_node *copy;
	struct vm_cell *roots[3];
	unsigned index;
	int error;

	/* Invalid embedding arguments cannot allocate or publish a partial copy. */
	if (realm == NULL ||
	    source == NULL ||
	    created == NULL)
		return EINVAL;

	/* Copies share the actual source Document and its collector. */
	if (source->document == NULL || source->document->heap != realm->heap)
		return EINVAL;

	/* Source, linked copy and pending unlinked copy survive every native allocation. */
	roots[0] = &source->cell;
	roots[1] = NULL;
	roots[2] = NULL;

	/* Registers each stack slot before any shallow node factory may collect. */
	for (index = 0; index < 3U; index++) {
		error = vm_heap_add_root(realm->heap, &roots[index]);
		if (error != 0) {
			clone_unroot(realm->heap, roots, index);
			return error;
		}
	}

	/* The first complete node becomes the strongly traced root of the destination graph. */
	error = clone_shallow(source, &roots[2], &copy);
	if (error != 0) {
		clone_unroot(realm->heap, roots, 3);
		return error;
	}

	/* The top copy now traces every child linked during descent. */
	roots[1] = &copy->cell;

	/* Iterative descent cannot silently omit a deep suffix or overflow the C call stack. */
	if (deep) {
		error = clone_descendants(source, copy, &roots[2]);
		if (error != 0) {
			clone_unroot(realm->heap, roots, 3);
			return error;
		}
	}

	/* Publish only the complete graph while it is still retained by this invocation. */
	*created = copy;
	clone_unroot(realm->heap, roots, 3);

	/* Succeeded: ownership passes to the caller without an intervening allocation. */
	return 0;
}

/* Copies one native node, retaining it before attribute or form-state initialization. */
static int
clone_shallow(
	struct dom_node *source,
	struct vm_cell **pending,
	struct dom_node **created)
{
	struct dom_element *element;
	struct dom_element *copy_element;
	struct dom_character_data *text;
	struct dom_doctype *doctype;
	struct dom_node *copy;
	size_t index;
	int error;

	/* Every supported factory completes native identity before another VM allocation. */
	switch (source->type) {
	case DOM_ELEMENT:
		element = (struct dom_element *)source;
		copy_element = dom_element_create(source->document, element->ns, element->local_name, element->prefix);
		if (copy_element == NULL)
			return ENOMEM;

		/* Pending ownership covers initialization before the node joins the copy graph. */
		copy = &copy_element->node;
		*pending = &copy->cell;
		copy_element->namespace_uri = element->namespace_uri;

		/* Native attributes preserve exact identity independently of script expandos. */
		for (index = 0; index < element->attribute_count; index++) {
			error = dom_element_add_attribute(
			    copy_element,
			    element->attributes[index].ns,
			    element->attributes[index].prefix,
			    element->attributes[index].name,
			    element->attributes[index].value);
			if (error != 0)
				return error;

			/* The traced copy retains the exact attribute URI before the next allocation. */
			copy_element->attributes[index].namespace_uri = element->attributes[index].namespace_uri;
		}

		/* Immutable inline declarations keep their precision; later mutations replace only the owning element's source edge. */
		copy_element->inline_style_source = element->inline_style_source;

		/* Dirty input text and caret keep their established independent-buffer semantics. */
		error = dom_input_clone_value(copy_element, element);
		if (error != 0)
			return error;

		/* Checkedness copies without touching the original radio group or option dirtiness. */
		error = dom_input_clone_checked(copy_element, element);
		if (error != 0)
			return error;
		break;
	case DOM_TEXT:
		text = (struct dom_character_data *)source;
		copy = dom_text_create(source->document, text->data.data, text->data.length);
		break;
	case DOM_CDATA_SECTION:
		text = (struct dom_character_data *)source;
		error = dom_cdata_create(source->document, text->data.data, text->data.length, &copy);
		if (error != 0)
			return error;
		break;
	case DOM_PROCESSING_INSTRUCTION:
		text = (struct dom_character_data *)source;
		error = dom_pi_create(source->document, text->target, text->data.data, text->data.length, &copy);
		if (error != 0)
			return error;
		break;
	case DOM_COMMENT:
		text = (struct dom_character_data *)source;
		copy = dom_comment_create(source->document, text->data.data, text->data.length);
		break;
	case DOM_DOCUMENT_TYPE:
		doctype = (struct dom_doctype *)source;
		copy = dom_doctype_create(source->document, doctype->name, doctype->public_id, doctype->system_id);
		break;
	case DOM_DOCUMENT_FRAGMENT:
		copy = dom_fragment_create(source->document);
		break;
	default:
		/* Unsupported native kinds must not masquerade as empty DocumentFragments. */
		return EINVAL;
	}

	/* Failed native allocation leaves the caller's output unpublished. */
	if (copy == NULL)
		return ENOMEM;

	/* A complete shallow copy is retained before the next factory or template allocation. */
	*pending = &copy->cell;
	*created = copy;

	/* Succeeded: the native node has independently initialized data and control state. */
	return 0;
}

/* Descends complete subtrees with checked frames instead of a recursive depth cutoff. */
static int
clone_descendants(
	struct dom_node *source,
	struct dom_node *copy,
	struct vm_cell **pending)
{
	struct wb_vector frames;
	struct clone_frame initial;
	struct clone_frame child_frame;
	struct clone_frame *frame;
	struct dom_node *child;
	struct dom_node *child_copy;
	struct dom_node *contents;
	struct dom_element *element;
	int error;

	/* The first frame owns ordinary traversal followed by any separate template contents. */
	wb_vector_init(&frames, sizeof(struct clone_frame));
	initial.source = source;
	initial.copy = copy;
	initial.destination = copy;
	initial.next = source->first_child;
	initial.content_pending = 1;

	/* Publishes the first native traversal record before descending into either graph. */
	error = wb_vector_push(&frames, &initial);
	if (error != 0) {
		wb_vector_release(&frames);
		return error;
	}

	/* Linked copies and original participants remain traced even when the C vector reallocates. */
	while (frames.count != 0) {
		frame = wb_vector_at(&frames, frames.count - 1U);
		child = frame->next;

		/* Separate template fragments follow ordinary children once per element. */
		if (child == NULL && frame->content_pending) {
			frame->content_pending = 0;

			/* Other native kinds have no independent content graph. */
			if (frame->source->type == DOM_ELEMENT) {
				element = (struct dom_element *)frame->source;

				/* A lazily absent source fragment represents no content children. */
				if (element->content != NULL) {
					error = bind_template_contents((struct dom_element *)frame->copy, &contents);
					if (error != 0) {
						wb_vector_release(&frames);
						return error;
					}

					/* The copy element traces its newly installed template fragment. */
					frame->destination = contents;
					frame->next = element->content->first_child;
					continue;
				}
			}
		}

		/* Exhausted subtrees return to the preceding frame without C recursion. */
		if (child == NULL) {
			wb_vector_pop(&frames);
			continue;
		}

		/* Prepare the following sibling before another frame can move vector storage. */
		frame->next = child->next;
		error = clone_shallow(child, pending, &child_copy);
		if (error != 0) {
			wb_vector_release(&frames);
			return error;
		}

		/* The new graph retains this child before any subsequent VM allocation. */
		dom_append_child(frame->destination, child_copy);

		/* Prepares this child's independent ordinary-then-template traversal record. */
		child_frame.source = child;
		child_frame.copy = child_copy;
		child_frame.destination = child_copy;
		child_frame.next = child->first_child;
		child_frame.content_pending = 1;

		/* A checked push preserves every remaining subtree across vector growth. */
		error = wb_vector_push(&frames, &child_frame);
		if (error != 0) {
			wb_vector_release(&frames);
			return error;
		}
	}

	/* Release only the native traversal storage; the caller retains the complete copy. */
	wb_vector_release(&frames);

	/* Succeeded: every ordinary and separate content child has been copied in order. */
	return 0;
}

/* Releases exactly the temporary graph slots registered by this invocation. */
static void
clone_unroot(
	struct vm_heap *heap,
	struct vm_cell **roots,
	unsigned count)
{
	/* Reverse removal also covers partially registered root arrays. */
	while (count != 0) {
		count--;
		vm_heap_remove_root(heap, &roots[count]);
	}

	/* Succeeded: no temporary graph slot points into this invocation's expired stack. */
	return;
}
