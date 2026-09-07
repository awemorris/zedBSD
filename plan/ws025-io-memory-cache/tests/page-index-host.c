/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <kern/vm-object.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "src/kern/vm-object-index.inc"

#define COUNT 8192U
static struct vm_object_page pages[COUNT];
static unsigned char present[COUNT];
static unsigned visited;
static unsigned checks;
void hal_fatal(const char *file, int line, const char *message)
{ fprintf(stderr, "%s:%d %s\n", file, line, message); abort(); }
static unsigned verify(struct vm_object_page *page, off_t lower, off_t upper)
{
	unsigned left, right, id;
	if (page == NULL) return 0;
	assert(page->offset > lower && page->offset < upper);
	id = (unsigned)(page->offset / 4096);
	assert(id < COUNT && page == &pages[id] && present[id]);
	left = verify(page->index_left, lower, page->offset);
	right = verify(page->index_right, page->offset, upper);
	assert(left <= right + 1 && right <= left + 1);
	assert(page->index_height == 1 + (left > right ? left : right));
	visited++;
	checks++;
	return page->index_height;
}
static void invariant(struct vm_object *object)
{
	unsigned i, count = 0;
	visited = 0;
	assert(verify(object->page_index, -1, (off_t)COUNT * 4096) < 24);
	for (i = 0; i < COUNT; i++) count += present[i];
	assert(visited == count);
}
int main(void)
{
	struct vm_object object;
	unsigned round, i, id;
	uint32_t random = 71;
	memset(&object, 0, sizeof(object));
	for (i = 0; i < COUNT; i++) pages[i].offset = (off_t)i * 4096;
	for (round = 0; round < 3; round++) {
		for (i = 0; i < COUNT; i++) {
			id = round == 0 ? i : round == 1 ? COUNT - i - 1 : (i * 4051U) % COUNT;
			object_page_index_insert(&object, &pages[id]);
			present[id] = 1;
			if (i % 64 == 0) invariant(&object);
		}
		invariant(&object);
		for (i = 0; i < COUNT; i++) {
			id = (i * 4051U) % COUNT;
			object_page_index_remove(&object, &pages[id]);
			present[id] = 0;
			assert(pages[id].index_height == 0 && !pages[id].index_left && !pages[id].index_right);
			if (i % 64 == 0) invariant(&object);
		}
		invariant(&object);
	}
	for (i = 0; i < 65536; i++) {
		random = random * 1664525U + 1013904223U;
		id = (random >> 8) % COUNT;
		if (present[id]) object_page_index_remove(&object, &pages[id]);
		else object_page_index_insert(&object, &pages[id]);
		present[id] ^= 1;
		if (i % 64 == 0) invariant(&object);
	}
	invariant(&object);
	printf("page index PASS: %u nodes checked; ordered/reverse/random insertion, deletion, reinsertion\n", checks);
	return 0;
}
