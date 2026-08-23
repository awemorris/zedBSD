/* SUSv4 hash, linear, queue, and tree search interfaces. SPDX-License-Identifier: Zlib */
#include <errno.h>
#include <search.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static ENTRY *hash_entries;
static unsigned char *hash_used;
static size_t hash_capacity;

static size_t
hash_string(const char *key)
{
	size_t value = (size_t)2166136261U;
	while (*key != '\0') {
		value ^= (unsigned char)*key++;
		value *= (size_t)16777619U;
	}
	return value;
}

int
hcreate(size_t count)
{
	size_t capacity = 8;

	if (hash_entries != NULL) {
		errno = EINVAL;
		return 0;
	}
	if (count > SIZE_MAX / 2) {
		errno = ENOMEM;
		return 0;
	}
	while (capacity < count * 2) {
		if (capacity > SIZE_MAX / 2) {
			errno = ENOMEM;
			return 0;
		}
		capacity *= 2;
	}
	hash_entries = calloc(capacity, sizeof(*hash_entries));
	hash_used = calloc(capacity, sizeof(*hash_used));
	if (hash_entries == NULL || hash_used == NULL) {
		free(hash_entries);
		free(hash_used);
		hash_entries = NULL;
		hash_used = NULL;
		errno = ENOMEM;
		return 0;
	}
	hash_capacity = capacity;
	return 1;
}

void
hdestroy(void)
{
	free(hash_entries);
	free(hash_used);
	hash_entries = NULL;
	hash_used = NULL;
	hash_capacity = 0;
}

ENTRY *
hsearch(ENTRY item, ACTION action)
{
	size_t index, checked;

	if (hash_entries == NULL || item.key == NULL) {
		errno = EINVAL;
		return NULL;
	}
	if (action != FIND && action != ENTER) {
		errno = EINVAL;
		return NULL;
	}
	index = hash_string(item.key) & (hash_capacity - 1);
	for (checked = 0; checked < hash_capacity; checked++) {
		if (!hash_used[index]) {
			if (action == FIND)
				return NULL;
			hash_used[index] = 1;
			hash_entries[index] = item;
			return &hash_entries[index];
		}
		if (strcmp(hash_entries[index].key, item.key) == 0)
			return &hash_entries[index];
		index = (index + 1) & (hash_capacity - 1);
	}
	errno = ENOMEM;
	return NULL;
}

struct queue_link { struct queue_link *next, *previous; };

void
insque(void *element, void *predecessor)
{
	struct queue_link *item = element, *pred = predecessor;
	if (pred == NULL) {
		item->next = NULL;
		item->previous = NULL;
		return;
	}
	item->previous = pred;
	item->next = pred->next;
	if (item->next != NULL)
		item->next->previous = item;
	pred->next = item;
}

void
remque(void *element)
{
	struct queue_link *item = element;
	if (item->previous != NULL)
		item->previous->next = item->next;
	if (item->next != NULL)
		item->next->previous = item->previous;
}

void *
lfind(const void *key, const void *base, size_t *count, size_t width,
    int (*compare)(const void *, const void *))
{
	const unsigned char *bytes = base;
	size_t index;

	if (count == NULL || compare == NULL || (width == 0 && *count != 0))
		return NULL;
	for (index = 0; index < *count; index++)
		if (compare(key, bytes + index * width) == 0)
			return (void *)(bytes + index * width);
	return NULL;
}

void *
lsearch(const void *key, void *base, size_t *count, size_t width,
    int (*compare)(const void *, const void *))
{
	unsigned char *bytes = base;
	void *found = lfind(key, base, count, width, compare);
	if (found != NULL)
		return found;
	if (count == NULL || width == 0 || *count == SIZE_MAX ||
	    *count > SIZE_MAX / width) {
		errno = EOVERFLOW;
		return NULL;
	}
	found = bytes + *count * width;
	memcpy(found, key, width);
	(*count)++;
	return found;
}

struct tree_node {
	const void *key;
	struct tree_node *left, *right;
};

void *
tfind(const void *key, void *const *rootp,
    int (*compare)(const void *, const void *))
{
	struct tree_node *node;
	int order;
	if (rootp == NULL || compare == NULL)
		return NULL;
	node = *(struct tree_node *const *)rootp;
	while (node != NULL) {
		order = compare(key, node->key);
		if (order == 0)
			return node;
		node = order < 0 ? node->left : node->right;
	}
	return NULL;
}

void *
tsearch(const void *key, void **rootp,
    int (*compare)(const void *, const void *))
{
	struct tree_node **link, *node;
	int order;
	if (rootp == NULL || compare == NULL) {
		errno = EINVAL;
		return NULL;
	}
	link = (struct tree_node **)rootp;
	while (*link != NULL) {
		order = compare(key, (*link)->key);
		if (order == 0)
			return *link;
		link = order < 0 ? &(*link)->left : &(*link)->right;
	}
	node = malloc(sizeof(*node));
	if (node == NULL)
		return NULL;
	node->key = key;
	node->left = node->right = NULL;
	*link = node;
	return node;
}

void *
tdelete(const void *key, void **rootp,
    int (*compare)(const void *, const void *))
{
	struct tree_node **link, *node, *parent = NULL;
	if (rootp == NULL || compare == NULL)
		return NULL;
	link = (struct tree_node **)rootp;
	while (*link != NULL) {
		int order = compare(key, (*link)->key);
		if (order == 0)
			break;
		parent = *link;
		link = order < 0 ? &(*link)->left : &(*link)->right;
	}
	if (*link == NULL)
		return NULL;
	node = *link;
	if (node->left == NULL)
		*link = node->right;
	else if (node->right == NULL)
		*link = node->left;
	else {
		struct tree_node **successor = &node->right;
		while ((*successor)->left != NULL)
			successor = &(*successor)->left;
		node->key = (*successor)->key;
		{
			struct tree_node *old = *successor;
			*successor = old->right;
			free(old);
		}
		return parent != NULL ? parent : *rootp;
	}
	free(node);
	return parent != NULL ? parent : *rootp;
}

static void
walk_node(const struct tree_node *node,
    void (*action)(const void *, VISIT, int), int depth)
{
	if (node->left == NULL && node->right == NULL) {
		action(node, leaf, depth);
		return;
	}
	action(node, preorder, depth);
	if (node->left != NULL)
		walk_node(node->left, action, depth + 1);
	action(node, postorder, depth);
	if (node->right != NULL)
		walk_node(node->right, action, depth + 1);
	action(node, endorder, depth);
}

void
twalk(const void *root, void (*action)(const void *, VISIT, int))
{
	if (root != NULL && action != NULL)
		walk_node(root, action, 0);
}
