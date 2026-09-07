/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Intrusive dirty membership under the existing object lock. Hardware PTE dirt
 * still requires the normal reverse-map discovery pass before synchronization.
 */

/* Indexes resident dirty data, excluding resize-orphaned storage. */
static void
object_page_dirty_link(
	struct vm_object_page *page)
{
	/* Leaves clean, already indexed and orphaned descriptors out of publication. */
	if (page->dirty_linked || (page->flags & VM_OBJECT_PAGE_DIRTY) == 0 ||
	    (page->flags & VM_OBJECT_PAGE_ORPHANED) != 0)
		return;

	/* Adds membership without scanning or sorting other resident pages. */
	page->dirty_previous = NULL;
	page->dirty_next = page->owner->dirty_pages;
	if (page->dirty_next != NULL)
		page->dirty_next->dirty_previous = page;
	page->owner->dirty_pages = page;
	page->dirty_linked = 1;
}

/* Removes writeback membership before a clean transition, orphan or retirement. */
static void
object_page_dirty_unlink(
	struct vm_object_page *page)
{
	/* Accepts never-dirty and already detached pages without touching their owner. */
	if (!page->dirty_linked)
		return;

	/* Repairs the owner list before the descriptor may leave its resident index. */
	if (page->dirty_previous != NULL)
		page->dirty_previous->dirty_next = page->dirty_next;
	else
		page->owner->dirty_pages = page->dirty_next;
	if (page->dirty_next != NULL)
		page->dirty_next->dirty_previous = page->dirty_previous;
	page->dirty_previous = NULL;
	page->dirty_next = NULL;
	page->dirty_linked = 0;
}

/* Advances dirty content and publishes eligible membership as one owner transition. */
static void
object_page_dirty_mark(
	struct vm_object_page *page)
{
	object_prefetch_retire(page);
	page->flags |= VM_OBJECT_PAGE_DIRTY;
	page->dirty_generation = next_generation(page->owner);
	object_page_dirty_link(page);
}
