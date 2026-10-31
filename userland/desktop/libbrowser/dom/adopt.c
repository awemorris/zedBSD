/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Checked adoption changes current Document ownership without replacing native nodes or wrappers. */

#include "dom/dom.h"

#include <errno.h>

static int adopt_prepare(struct dom_document *document, struct dom_node *node);
static int adopt_tree(struct dom_document *document, struct dom_node *node);

/*
 * Adopts a native subtree into a same-heap Document after validating its complete graph.
 * Original pre-removal notifications run before any current Document changes.
 */
int
dom_adopt(
	struct dom_document *document,
	struct dom_node *node)
{
	int status;

	/* Invalid native arguments cannot create a partial ownership change. */
	if (document == NULL || node == NULL)
		return EINVAL;

	/* All subtree heap checks and prospective registry allocations precede unlinking. */
	status = adopt_prepare(document, node);
	if (status != 0)
		return status;

	/* Original Document observers repair references while every old link still exists. */
	dom_remove(node);

	/* Transfers the prepared subtree without replacing its native identities. */
	status = adopt_tree(document, node);
	if (status != 0)
		return status;

	/* Succeeded: nodes and rooted weak iterator subscriptions belong to the current Document. */
	return 0;
}

/* Validates descendants and preallocates a destination registry without changing DOM links. */
static int
adopt_prepare(
	struct dom_document *document,
	struct dom_node *node)
{
	struct dom_node *child;
	struct dom_element *element;
	int status;

	/* Native heaps cannot share collectible cells, and Document nodes cannot be adopted. */
	if (node->document->heap != document->heap || node->type == DOM_DOCUMENT)
		return EINVAL;

	/* A possible iterator migration performs its only allocation before the first removal. */
	if (node->document != document && node->document->removals != NULL) {
		status = dom_removal_prepare(document);
		if (status != 0)
			return status;
	}

	/* Descendants may expose an old heterogeneous tree produced before adoption was supported. */
	for (child = node->first_child;
	     child != NULL;
	     child = child->next) {
		status = adopt_prepare(document, child);
		if (status != 0)
			return status;
	}

	/* Existing template content graphs use this engine's same-Document fragment ownership model. */
	if (node->type == DOM_ELEMENT) {
		element = (struct dom_element *)node;

		/* Validate the separately owned template fragment before any node is unlinked. */
		if (element->content != NULL) {
			status = adopt_prepare(document, element->content);
			if (status != 0)
				return status;
		}
		if (element->shadow_root != NULL) {
			status = adopt_prepare(document, element->shadow_root);
			if (status != 0)
				return status;
		}
	}

	/* Succeeded: committing this graph needs no later registry allocation. */
	return 0;
}

/* Transfers weak rooted tokens and changes the node Document without invoking script or GC. */
static int
adopt_tree(
	struct dom_document *document,
	struct dom_node *node)
{
	struct dom_node *child;
	struct dom_element *element;
	int status;

	/* Native wrappers and cache identities remain intact while their current owner changes. */
	if (node->document != document) {
		status = dom_removal_move_root(node, document);
		if (status != 0)
			return status;

		/* Publish the owner only after all subscriptions follow the destination. */
		node->document = document;
	}

	/* Each actual descendant receives the new current Document before later host callbacks. */
	for (child = node->first_child;
	     child != NULL;
	     child = child->next) {
		status = adopt_tree(document, child);
		if (status != 0)
			return status;
	}

	/* Nested template content fragments follow their existing owner model without entering tree order. */
	if (node->type == DOM_ELEMENT) {
		element = (struct dom_element *)node;

		/* Transfer template storage even though ordinary child traversal cannot reach it. */
		if (element->content != NULL) {
			status = adopt_tree(document, element->content);
			if (status != 0)
				return status;
		}
		if (element->shadow_root != NULL) {
			status = adopt_tree(document, element->shadow_root);
			if (status != 0)
				return status;
		}
	}

	/* Succeeded: this node and its complete existing content graph use the destination Document. */
	return 0;
}
