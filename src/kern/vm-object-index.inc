/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Intrusive AVL page index. The object lock protects every tree mutation.
 * Rotations move links, never descriptors held by faults or mapping pins.
 * Balanced recursion is logarithmic in the number of resident pages.
 */

static unsigned page_index_height(const struct vm_object_page *page);
static void page_index_update(struct vm_object_page *page);
static struct vm_object_page *page_index_rotate_left(struct vm_object_page *page);
static struct vm_object_page *page_index_rotate_right(struct vm_object_page *page);
static struct vm_object_page *page_index_balance(struct vm_object_page *page);
static struct vm_object_page *page_index_insert(struct vm_object_page *root, struct vm_object_page *page);
static struct vm_object_page *page_index_extract_min(struct vm_object_page *root, struct vm_object_page **minimum);
static struct vm_object_page *page_index_remove(struct vm_object_page *root, struct vm_object_page *page);

/* Reports the empty subtree height without dereferencing a missing child. */
static unsigned
page_index_height(
	const struct vm_object_page *page)
{
	if (page == NULL)
		return 0;
	return page->index_height;
}

/* Refreshes one node after its children change. */
static void
page_index_update(
	struct vm_object_page *page)
{
	unsigned left;
	unsigned right;

	/* Derives height from already balanced child subtrees. */
	left = page_index_height(page->index_left);
	right = page_index_height(page->index_right);
	page->index_height = 1U + (left > right ? left : right);
}

/* Rotates a right-heavy subtree without changing page identity. */
static struct vm_object_page *
page_index_rotate_left(
	struct vm_object_page *page)
{
	struct vm_object_page *root;

	/* Transfers the middle subtree before refreshing both heights. */
	root = page->index_right;
	page->index_right = root->index_left;
	root->index_left = page;
	page_index_update(page);
	page_index_update(root);
	return root;
}

/* Rotates a left-heavy subtree without changing page identity. */
static struct vm_object_page *
page_index_rotate_right(
	struct vm_object_page *page)
{
	struct vm_object_page *root;

	/* Transfers the middle subtree before refreshing both heights. */
	root = page->index_left;
	page->index_left = root->index_right;
	root->index_right = page;
	page_index_update(page);
	page_index_update(root);
	return root;
}

/* Restores the AVL height bound after one insertion or removal. */
static struct vm_object_page *
page_index_balance(
	struct vm_object_page *page)
{
	unsigned left;
	unsigned right;

	/* Measures the changed children before choosing a rotation. */
	page_index_update(page);
	left = page_index_height(page->index_left);
	right = page_index_height(page->index_right);

	/* Corrects a left-heavy subtree, including its inner-heavy case. */
	if (left > right + 1U) {
		if (page_index_height(page->index_left->index_right) >
		    page_index_height(page->index_left->index_left))
			page->index_left = page_index_rotate_left(page->index_left);
		page = page_index_rotate_right(page);
	} else if (right > left + 1U) {
		if (page_index_height(page->index_right->index_left) >
		    page_index_height(page->index_right->index_right))
			page->index_right = page_index_rotate_right(page->index_right);
		page = page_index_rotate_left(page);
	}

	/* Returns the balanced subtree root to its parent. */
	return page;
}

/* Inserts one unique page while preserving ordered child keys. */
static struct vm_object_page *
page_index_insert(
	struct vm_object_page *root,
	struct vm_object_page *page)
{
	/* Publishes a new leaf or rejects an inconsistent duplicate owner. */
	if (root == NULL)
		return page;
	if (root->offset == page->offset)
		HAL_FATAL("duplicate VM object page index key");

	/* Descends through one subtree and repairs heights on return. */
	if (page->offset < root->offset)
		root->index_left = page_index_insert(root->index_left, page);
	else
		root->index_right = page_index_insert(root->index_right, page);
	root = page_index_balance(root);
	return root;
}

/* Extracts the successor node itself, preserving all descriptor identities. */
static struct vm_object_page *
page_index_extract_min(
	struct vm_object_page *root,
	struct vm_object_page **minimum)
{
	/* Removes the smallest node and returns its surviving right subtree. */
	if (root->index_left == NULL) {
		*minimum = root;
		return root->index_right;
	}

	/* Repairs ancestors after unlinking the successor. */
	root->index_left = page_index_extract_min(root->index_left, minimum);
	root = page_index_balance(root);
	return root;
}

/* Removes the exact descriptor without copying another page's contents. */
static struct vm_object_page *
page_index_remove(
	struct vm_object_page *root,
	struct vm_object_page *page)
{
	struct vm_object_page *successor;
	struct vm_object_page *right;

	/* Detects list/index divergence at the mutation which caused it. */
	if (root == NULL)
		HAL_FATAL("missing VM object page index key");

	/* Finds the exact key, preserving a balanced path after removal. */
	if (page->offset < root->offset) {
		root->index_left = page_index_remove(root->index_left, page);
	} else if (page->offset > root->offset) {
		root->index_right = page_index_remove(root->index_right, page);
	} else {
		if (root != page)
			HAL_FATAL("VM object page index identity mismatch");
		if (root->index_left == NULL)
			return root->index_right;
		if (root->index_right == NULL)
			return root->index_left;
		right = page_index_extract_min(root->index_right, &successor);
		successor->index_left = root->index_left;
		successor->index_right = right;
		root = successor;
	}
	root = page_index_balance(root);
	return root;
}

/* Publishes a descriptor under the already held object lock. */
static void
object_page_index_insert(
	struct vm_object *object,
	struct vm_object_page *page)
{
	/* Starts with an independent leaf before linking it into the object. */
	page->index_left = NULL;
	page->index_right = NULL;
	page->index_height = 1;
	object->page_index = page_index_insert(object->page_index, page);
}

/* Detaches a descriptor before it becomes an orphan or is retired. */
static void
object_page_index_remove(
	struct vm_object *object,
	struct vm_object_page *page)
{
	/* Removes the key and clears links before another owner can reuse them. */
	object->page_index = page_index_remove(object->page_index, page);
	page->index_left = NULL;
	page->index_right = NULL;
	page->index_height = 0;
}
