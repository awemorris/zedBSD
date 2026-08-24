#include "libc/heap.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int
main(void)
{
	static uint8_t kernel_storage[4096];
	static uint8_t noct_storage[4096];
	struct heap_allocator kernel_heap, noct_heap;
	struct heap_allocator *previous;
	char *persistent, *temporary;

	heap_allocator_init(&kernel_heap, kernel_storage,
				 sizeof(kernel_storage));
	heap_allocator_init(&noct_heap, noct_storage, sizeof(noct_storage));
	previous = heap_active_set(&kernel_heap);
	persistent = heap_alloc_active(64);
	assert(persistent != NULL);
	memcpy(persistent, "persistent", 11);

	assert(heap_active_set(&noct_heap) == &kernel_heap);
	temporary = heap_alloc_active(128);
	assert(temporary != NULL);
	memset(temporary, 0xa5, 128);
	assert(heap_allocator_current(&kernel_heap) == 64);
	assert(heap_allocator_current(&noct_heap) == 128);
	{
		void *aligned = heap_aligned_alloc_active(256, 512);

		assert(aligned != NULL);
		assert(((uintptr_t)aligned & 255U) == 0);
		memset(aligned, 0x5a, 512);
		assert(heap_allocator_validate(&noct_heap));
		heap_free_active(aligned);
		assert(heap_allocator_validate(&noct_heap));
	}

	heap_allocator_reset(&noct_heap);
	assert(heap_allocator_current(&noct_heap) == 0);
	assert(memcmp(persistent, "persistent", 11) == 0);
	assert(heap_allocator_validate(&kernel_heap));
	assert(heap_allocator_validate(&noct_heap));

	assert(heap_active_set(&kernel_heap) == &noct_heap);
	heap_free_active(persistent);
	assert(heap_allocator_current(&kernel_heap) == 0);
	(void)heap_active_set(previous);
	puts("zedBSD heap-context isolation tests: PASS");
	return 0;
}
