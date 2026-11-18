/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Weak mutation subscriptions repair traversal positions before unlinking and
 * live Range boundaries after complete insertion or CharacterData replacement. Tokens
 * and their registry are C allocations so either GC finalization order is safe.
 * A subscriber finalizer must unsubscribe before its weak context is freed.
 */

#include "dom/dom.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>

/* One token weakly names a subscriber until its own explicit cleanup. */
struct dom_removal_subscription {
	struct dom_removal_registry *registry;
	struct dom_removal_subscription *previous;
	struct dom_removal_subscription *next;
	/* A rooted subscriber owns this node strongly; the C token never traces it. */
	struct dom_node *root;
	void *context;
	void (*notify)(void *context, struct dom_node *node);
	/* Optional live-boundary repairs share this token's weak context and ownership. */
	void (*inserted)(void *context, struct dom_node *node);
	void (*data)(void *context, struct dom_node *node, size_t offset, size_t removed, size_t added);
	/* Attached Text splitting repairs containers after ordinary insertion repair. */
	void (*split)(void *context, struct dom_node *node, struct dom_node *created, uint32_t offset);
};

/* Document and token references retain this FIFO without rooting any GC context. */
struct dom_removal_registry {
	struct dom_removal_subscription *first;
	struct dom_removal_subscription *last;
	size_t references;
	int notifying;
};

static void removal_drop(struct dom_removal_registry *registry);

/*
 * Registers a weak C callback before a Document's nodes are unlinked.
 */
int
dom_removal_subscribe(
	struct dom_document *document,
	void *context,
	void (*notify)(void *context, struct dom_node *node),
	struct dom_removal_subscription **subscription)
{
	struct dom_removal_registry *registry;
	struct dom_removal_subscription *entry;
	int created;

	/* Invalid embedding arguments cannot establish a cleanup obligation. */
	if (document == NULL ||
	    notify == NULL ||
	    subscription == NULL)
		return EINVAL;

	/* A new registry begins with only the Document's ownership reference. */
	registry = document->removals;
	created = 0;
	if (registry == NULL) {
		registry = calloc(1, sizeof(*registry));
		if (registry == NULL)
			return ENOMEM;

		/* The unpublished registry has exactly its prospective Document ownership. */
		registry->references = 1;
		created = 1;
	}

	/* Callbacks may not modify the subscription list they are traversing. */
	if (registry->notifying)
		return EBUSY;

	/* An overflowing ownership counter cannot safely admit another token. */
	if (registry->references == SIZE_MAX)
		return EOVERFLOW;

	/* Token allocation failure leaves the existing registry and output untouched. */
	entry = calloc(1, sizeof(*entry));
	if (entry == NULL) {
		if (created)
			free(registry);
		return ENOMEM;
	}

	/* This complete token owns one registry reference and weakly names its context. */
	entry->registry = registry;
	entry->context = context;
	entry->notify = notify;
	entry->previous = registry->last;
	registry->references++;

	/* Publishing in registration order never exposes a partial token. */
	if (registry->last != NULL) {
		registry->last->next = entry;
	} else {
		registry->first = entry;
	}

	/* The Document now owns even a registry created for its first subscriber. */
	registry->last = entry;
	document->removals = registry;
	*subscription = entry;

	/* Succeeded: the caller owns the token and must release it before its context dies. */
	return 0;
}

/*
 * Releases one weak token without reading its Document or subscriber context.
 */
void
dom_removal_unsubscribe(
	struct dom_removal_subscription *subscription)
{
	struct dom_removal_registry *registry;

	/* Optional or failed construction has no token to release. */
	if (subscription == NULL)
		return;

	/* List mutation is forbidden while a synchronous repair callback is active. */
	registry = subscription->registry;
	assert(!registry->notifying);

	/* Both neighbouring entries are separately allocated until their own unlink. */
	if (subscription->previous != NULL) {
		subscription->previous->next = subscription->next;
	} else {
		registry->first = subscription->next;
	}

	/* The tail follows the same token-only ownership rule as the head. */
	if (subscription->next != NULL) {
		subscription->next->previous = subscription->previous;
	} else {
		registry->last = subscription->previous;
	}

	/* Dropping the token reference may free a registry whose Document died first. */
	free(subscription);
	removal_drop(registry);

	/* Succeeded: no registry callback can reach the former weak context. */
	return;
}

/*
 * Notifies registered C repair callbacks while the removed node's links are intact.
 */
void
dom_removal_notify(
	struct dom_document *document,
	struct dom_node *node)
{
	struct dom_removal_registry *registry;
	struct dom_removal_subscription *entry;

	/* Documents without subscriptions incur no callback or allocation work. */
	registry = document->removals;
	if (registry == NULL)
		return;

	/* Repair callbacks cannot nest DOM mutation, allocation, GC or subscription changes. */
	assert(!registry->notifying);
	registry->notifying = 1;
	for (entry = registry->first; entry != NULL; entry = entry->next)
		entry->notify(entry->context, node);

	/* Finishing delivery restores permission for later subscriber finalizers to unlink. */
	registry->notifying = 0;

	/* Succeeded: all current weak subscribers saw the original removal links. */
	return;
}

/*
 * Configures optional live-boundary repairs without changing token ownership.
 */
void
dom_removal_set_updates(
	struct dom_removal_subscription *subscription,
	void (*inserted)(void *context, struct dom_node *node),
	void (*data)(void *context, struct dom_node *node, size_t offset, size_t removed, size_t added))
{
	/* Configuration occurs outside notification, before the complete subscriber is published. */
	assert(subscription != NULL);
	assert(!subscription->registry->notifying);
	subscription->inserted = inserted;
	subscription->data = data;

	/* Succeeded: generic removal observers remain unaffected when callbacks are absent. */
	return;
}

/*
 * Delivers pure insertion repair after the new child's links are complete.
 */
void
dom_insertion_notify(
	struct dom_document *document,
	struct dom_node *node)
{
	struct dom_removal_registry *registry;
	struct dom_removal_subscription *entry;

	/* A Document without weak subscribers requires no extra work. */
	registry = document->removals;
	if (registry == NULL)
		return;

	/* Optional callbacks obey the same non-nesting, allocation-free delivery contract. */
	assert(!registry->notifying);
	registry->notifying = 1;
	for (entry = registry->first; entry != NULL; entry = entry->next) {
		/* Traversal-only observers do not receive unrelated insertion work. */
		if (entry->inserted != NULL)
			entry->inserted(entry->context, node);
	}

	/* Completed delivery permits later mutation or subscriber finalization. */
	registry->notifying = 0;

	/* Succeeded: live subscribers saw the final links without changing the tree. */
	return;
}

/*
 * Delivers pure CharacterData repair after the replacement buffer is committed.
 */
void
dom_data_notify(
	struct dom_document *document,
	struct dom_node *node,
	size_t offset,
	size_t removed,
	size_t added)
{
	struct dom_removal_registry *registry;
	struct dom_removal_subscription *entry;

	/* A Document without weak subscribers requires no extra work. */
	registry = document->removals;
	if (registry == NULL)
		return;

	/* Repairs cannot allocate, collect, change the registry or start another DOM mutation. */
	assert(!registry->notifying);
	registry->notifying = 1;
	for (entry = registry->first; entry != NULL; entry = entry->next) {
		/* Removal-only observers retain their original callback behavior. */
		if (entry->data != NULL)
			entry->data(entry->context, node, offset, removed, added);
	}

	/* Completed delivery restores permission for later token cleanup. */
	registry->notifying = 0;

	/* Succeeded: native boundaries reflect the committed UTF16 replacement. */
	return;
}

/*
 * Configures optional Text-split repair without introducing a strong GC edge.
 */
void
dom_removal_set_split(
	struct dom_removal_subscription *subscription,
	void (*split)(void *context, struct dom_node *node, struct dom_node *created, uint32_t offset))
{
	/* Configuration precedes publication and cannot alter active notification. */
	assert(subscription != NULL);
	assert(!subscription->registry->notifying);
	subscription->split = split;

	/* Succeeded: the same weak context can repair attached split boundaries. */
	return;
}

/*
 * Delivers attached Text-split repair after insertion and before original-data truncation.
 */
void
dom_split_notify(
	struct dom_document *document,
	struct dom_node *node,
	struct dom_node *created,
	uint32_t offset)
{
	struct dom_removal_registry *registry;
	struct dom_removal_subscription *entry;

	/* A Document without weak subscribers has no split boundaries to repair. */
	registry = document->removals;
	if (registry == NULL)
		return;

	/* Native split callbacks obey the existing pure, non-nesting delivery contract. */
	assert(!registry->notifying);
	registry->notifying = 1;
	for (entry = registry->first; entry != NULL; entry = entry->next) {
		/* Ordinary traversal and data-only observers retain their original delivery behavior. */
		if (entry->split != NULL)
			entry->split(entry->context, node, created, offset);
	}

	/* Completed repair allows later mutation and subscriber finalization. */
	registry->notifying = 0;

	/* Succeeded: all configured live boundaries now name their actual split containers. */
	return;
}

/*
 * Releases the Document's registry ownership independently of token finalization.
 */
void
dom_removal_release(
	struct dom_document *document)
{
	struct dom_removal_registry *registry;

	/* Documents that never subscribed need no C cleanup. */
	registry = document->removals;
	if (registry == NULL)
		return;

	/* Only the Document's reference is dropped; remaining tokens still own their list. */
	document->removals = NULL;
	removal_drop(registry);

	/* Succeeded: tokens can finalize in any later order without accessing this Document. */
	return;
}

/*
 * Associates an existing weak subscription with its strongly owned traversal root.
 * Generic Document observers retain their original unassociated behavior.
 */
void
dom_removal_set_root(
	struct dom_removal_subscription *subscription,
	struct dom_node *root)
{
	/* Association occurs during native subscriber construction, before publication. */
	assert(subscription != NULL);
	assert(!subscription->registry->notifying);
	subscription->root = root;

	/* Succeeded: adoption can identify this root without a strong C registry edge. */
	return;
}

/*
 * Preallocates a destination registry before checked subtree adoption unlinks nodes.
 */
int
dom_removal_prepare(
	struct dom_document *document)
{
	struct dom_removal_registry *registry;

	/* A registry already owned by this Document needs no further preparation. */
	if (document->removals != NULL)
		return 0;

	/* Allocation failure leaves all original node links and token lists untouched. */
	registry = calloc(1, sizeof(*registry));
	if (registry == NULL)
		return ENOMEM;

	/* The complete empty registry begins with exactly its Document ownership reference. */
	registry->references = 1;
	document->removals = registry;

	/* Succeeded: token migration needs no allocation during ownership changes. */
	return 0;
}

/*
 * Transfers weak tokens rooted at one adopted node to its prepared destination Document.
 * Token identity survives moves, so subscriber finalizers keep their original cleanup handle.
 */
int
dom_removal_move_root(
	struct dom_node *root,
	struct dom_document *document)
{
	struct dom_removal_registry *original;
	struct dom_removal_registry *destination;
	struct dom_removal_subscription *entry;
	struct dom_removal_subscription *following;
	size_t count;

	/* Unassociated generic observers and unrelated roots remain in their original Document. */
	original = root->document->removals;
	if (original == NULL || root->document == document)
		return 0;

	/* Token migration cannot overlap its source registry's pure notification. */
	assert(!original->notifying);

	/* Counts this root's migration before changing either list or any ownership counter. */
	count = 0;
	for (entry = original->first; entry != NULL; entry = entry->next) {
		/* Only subscriptions strongly owned by this actual traversal root migrate. */
		if (entry->root == root)
			count++;
	}

	/* An original registry can contain only generic observers or unrelated iterator roots. */
	if (count == 0)
		return 0;

	/* Checked adoption has prepared this destination before unlinking any subtree. */
	destination = document->removals;
	if (destination == NULL)
		return EINVAL;

	/* Prepared destination ownership must be stable throughout this checked migration. */
	assert(!destination->notifying);

	/* The complete migration count must fit before any token or list link changes. */
	if (count > SIZE_MAX - destination->references)
		return EOVERFLOW;

	/* Each moved token exchanges one registry reference without allocation or context destruction. */
	for (entry = original->first; entry != NULL; entry = following) {
		/* Captures the old successor before any matching token changes lists. */
		following = entry->next;
		if (entry->root != root)
			continue;

		/* Unlinks exactly this token while all other original observer ordering remains intact. */
		if (entry->previous != NULL) {
			entry->previous->next = entry->next;
		} else {
			original->first = entry->next;
		}

		/* Both original ends continue to describe the remaining weak subscribers. */
		if (entry->next != NULL) {
			entry->next->previous = entry->previous;
		} else {
			original->last = entry->previous;
		}

		/* The same token now follows every previously registered destination subscriber. */
		entry->registry = destination;
		entry->previous = destination->last;
		entry->next = NULL;

		/* Registration order appends this token after all existing destination observers. */
		if (destination->last != NULL) {
			destination->last->next = entry;
		} else {
			destination->first = entry;
		}

		/* The destination owns this token reference before the original drops its reference. */
		destination->last = entry;
		destination->references++;
		removal_drop(original);
	}

	/* Succeeded: finalization and later pre-removal delivery use the token's current registry. */
	return 0;
}

/*
 * Tests a live Document's registry identity without retaining a raw Document in its weak token.
 */
int
dom_removal_matches_document(
	const struct dom_removal_subscription *subscription,
	const struct dom_document *document)
{
	/* An absent token cannot carry an association with this live Document. */
	if (subscription == NULL)
		return 0;

	/* A different or absent registry requires checked subscription replacement. */
	if (subscription->registry != document->removals)
		return 0;

	/* Succeeded: the active token belongs to this Document's independently owned registry. */
	return 1;
}

/*
 * Rebinds only a weak associated node during a pure native repair or boundary commit.
 * This does not modify the registry list, ownership counters or callback context.
 */
void
dom_removal_rebind_root(
	struct dom_removal_subscription *subscription,
	struct dom_node *root)
{
	/* The subscriber strongly owns the new node before changing this weak adoption association. */
	assert(subscription != NULL);
	assert(root != NULL);
	assert(subscription->registry == root->document->removals);
	subscription->root = root;

	/* Succeeded: adoption follows this actual current node even after a callback repaired it. */
	return;
}

/* Frees an empty registry only after its Document and all tokens release ownership. */
static void
removal_drop(
	struct dom_removal_registry *registry)
{
	/* Every caller drops exactly one previously acquired ownership reference. */
	assert(registry->references != 0);

	/* Remaining Document or token ownership keeps the independent C registry alive. */
	registry->references--;
	if (registry->references != 0)
		return;

	/* No weak contexts or neighbour links remain when the last owner disappears. */
	assert(registry->first == NULL);
	assert(registry->last == NULL);
	free(registry);

	/* Succeeded: the C registry has no remaining owners or allocations. */
	return;
}
