/* Private integrity checker must reject broken links without following them. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define ZEDBSD_KERNEL_HEAP_TRACE
#define malloc trace_test_malloc
#define calloc trace_test_calloc
#define realloc trace_test_realloc
#define free trace_test_free
#include "libc/heap.c"
#undef malloc
#undef calloc
#undef realloc
#undef free

int
main(void)
{
	static _Alignas(16) unsigned char bytes[8192];
	struct heap_allocator heap;
	struct heap_block *first;
	struct heap_block *next;
	struct heap_block fabricated;
	void *a;
	void *b;
	size_t capacity;

	heap_allocator_init(&heap, bytes, sizeof(bytes));
	assert(heap_allocator_trace_validate(&heap));
	a = heap_allocator_alloc(&heap, 73);
	b = heap_allocator_aligned_alloc(&heap, 64, 155);
	assert(a != NULL && b != NULL);
	assert(heap_allocator_trace_validate(&heap));
	first = heap.first;
	next = first->next_physical;
	first->next_physical = first;
	assert(!heap_allocator_trace_validate(&heap));
	first->next_physical = (struct heap_block *)(uintptr_t)1;
	assert(!heap_allocator_trace_validate(&heap));
	first->next_physical = next;
	capacity = first->capacity;
	first->capacity = SIZE_MAX;
	assert(!heap_allocator_trace_validate(&heap));
	first->capacity = capacity;
	first->capacity++;
	first->next_physical = (struct heap_block *)((unsigned char *)next + 1);
	assert(!heap_allocator_trace_validate(&heap));
	first->capacity = capacity;
	first->next_physical = next;
	assert(heap_allocator_trace_validate(&heap));
	heap_allocator_free(&heap, a);
	heap_allocator_free(&heap, b);
	assert(heap_allocator_trace_validate(&heap));
	first = heap.first;
	assert(first->state == HEAP_FREE);
	first->next_free = first;
	assert(!heap_allocator_trace_validate(&heap));
	first->next_free = NULL;
	memset(&fabricated, 0, sizeof(fabricated));
	fabricated.magic = HEAP_MAGIC;
	fabricated.state = HEAP_FREE;
	heap.free_list = &fabricated;
	assert(!heap_allocator_trace_validate(&heap));
	heap.free_list = first;
	first->previous_free = first;
	assert(!heap_allocator_trace_validate(&heap));
	first->previous_free = NULL;
	heap.free_list = NULL;
	assert(!heap_allocator_trace_validate(&heap));
	heap.free_list = first;
	heap_allocator_init(&heap, bytes, sizeof(bytes));
	a = heap_allocator_alloc(&heap, 73);
	b = heap_allocator_alloc(&heap, 73);
	assert(a != NULL && b != NULL);
	first = heap.first;
	next = first->next_physical;
	first->next_physical = first;
	heap_allocator_free(&heap, b);
	assert(heap.errors != 0);
	first->next_physical = next;
	assert(heap_allocator_trace_validate(&heap));
	puts("heap trace validator: healthy/aligned/physical/free-list/bounded-walk PASS");
	return 0;
}
