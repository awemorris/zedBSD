/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Native XML character nodes own exact data and target identity before any public binding exposure. */

#include "dom/dom.h"

#include <errno.h>

static int xml_character_create(struct dom_document *document, int type, struct vm_string *target, const uint16_t *units, size_t length, struct dom_node **created);

/*
 * Makes a native CDATA node from already validated caller-owned C data.
 */
int
dom_cdata_create(
	struct dom_document *document,
	const uint16_t *units,
	size_t length,
	struct dom_node **created)
{
	int status;

	/* The native factory preserves CDATA identity rather than converting it to ordinary Text. */
	status = xml_character_create(document, DOM_CDATA_SECTION, NULL, units, length, created);
	if (status != 0)
		return status;

	/* Succeeded: the caller owns a native CDATA node with no temporary roots left behind. */
	return 0;
}

/*
 * Makes a native processing instruction with an actual traced target and copied C data.
 */
int
dom_pi_create(
	struct dom_document *document,
	struct vm_string *target,
	const uint16_t *units,
	size_t length,
	struct dom_node **created)
{
	int status;

	/* Missing output storage cannot receive a completed processing instruction. */
	if (created == NULL)
		return EINVAL;

	/* Leave the output empty unless a validated native instruction is constructed. */
	*created = NULL;
	if (target == NULL)
		return EINVAL;

	/* Copy the character data while the validated native target stays rooted. */
	status = xml_character_create(document, DOM_PROCESSING_INSTRUCTION, target, units, length, created);
	if (status != 0)
		return status;

	/* Succeeded: the instruction owns its target independently of the caller's local. */
	return 0;
}

/*
 * Identifies the actual native character-data kinds without confusing them with child containers.
 */
int
dom_is_character_data(
	const struct dom_node *node)
{
	/* Missing nodes have no character buffer. */
	if (node == NULL)
		return 0;

	/* Every native CharacterData subtype stores content outside its child list. */
	if (node->type == DOM_TEXT ||
	    node->type == DOM_CDATA_SECTION ||
	    node->type == DOM_PROCESSING_INSTRUCTION ||
	    node->type == DOM_COMMENT)
		return 1;

	/* Succeeded: this remaining native kind is outside the CharacterData category. */
	return 0;
}

/*
 * Identifies Text and its CDATA subtype independently of other CharacterData nodes.
 */
int
dom_is_text(
	const struct dom_node *node)
{
	/* Only these two native kinds contribute text content. */
	if (node == NULL)
		return 0;

	/* CDATA preserves Text behavior while retaining its distinct native identity. */
	if (node->type == DOM_TEXT || node->type == DOM_CDATA_SECTION)
		return 1;

	/* Succeeded: this remaining native kind does not contribute Text content. */
	return 0;
}

/* Completes native character identity while actual Document and target survive allocation collection. */
static int
xml_character_create(
	struct dom_document *document,
	int type,
	struct vm_string *target,
	const uint16_t *units,
	size_t length,
	struct dom_node **created)
{
	struct dom_node *node;
	struct dom_character_data *data;
	struct vm_heap *heap;
	struct vm_cell *document_root;
	struct vm_cell *target_root;
	int status;

	/* Missing output storage cannot receive a completed native character node. */
	if (created == NULL)
		return EINVAL;

	/* Keep the caller's output empty if the requested owner is invalid. */
	*created = NULL;
	if (document == NULL)
		return EINVAL;

	/* Nonempty character data requires readable caller-owned input storage. */
	if (length != 0 && units == NULL)
		return EINVAL;

	/* Reserve one terminating unit without overflowing the copied UTF16 allocation. */
	if (length > (size_t)-1 / sizeof(*units) - 1U)
		return EOVERFLOW;

	/* The source is ordinary caller-owned C storage; only Document and target are VM edges here. */
	heap = document->heap;
	document_root = &document->node.cell;
	target_root = NULL;
	if (target != NULL)
		target_root = &target->cell;

	/* Protect the actual owner before the factory can trigger collection. */
	status = vm_heap_add_root(heap, &document_root);
	if (status != 0)
		return status;

	/* Protect an optional target with a nullable slot and undo the owner root on failure. */
	status = vm_heap_add_root(heap, &target_root);
	if (status != 0) {
		vm_heap_remove_root(heap, &document_root);
		return status;
	}

	/* Base character storage allocation may collect; complete target/type initialization cannot allocate again. */
	node = dom_text_create(document, units, length);
	if (node == NULL) {
		vm_heap_remove_root(heap, &target_root);
		vm_heap_remove_root(heap, &document_root);
		return ENOMEM;
	}

	/* The collector cell already uses the actual character-data tracer and C buffer finalizer. */
	data = (struct dom_character_data *)node;
	data->target = target;
	node->type = (uint16_t)type;
	*created = node;

	/* Release temporary protection after the complete node is available to the caller. */
	vm_heap_remove_root(heap, &target_root);
	vm_heap_remove_root(heap, &document_root);

	/* Succeeded: the caller receives a complete actual owner graph with no intervening allocation. */
	return 0;
}
