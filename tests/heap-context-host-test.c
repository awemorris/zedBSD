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
	struct zedbsd_heap kernel_heap, noct_heap;
	struct zedbsd_heap *previous;
	char *persistent, *temporary;

	zedbsd_heap_init_instance(&kernel_heap, kernel_storage,
				 sizeof(kernel_storage));
	zedbsd_heap_init_instance(&noct_heap, noct_storage, sizeof(noct_storage));
	previous = zedbsd_heap_set_active(&kernel_heap);
	persistent = zedbsd_malloc(64);
	assert(persistent != NULL);
	memcpy(persistent, "persistent", 11);

	assert(zedbsd_heap_set_active(&noct_heap) == &kernel_heap);
	temporary = zedbsd_malloc(128);
	assert(temporary != NULL);
	memset(temporary, 0xa5, 128);
	assert(zedbsd_heap_current_instance(&kernel_heap) == 64);
	assert(zedbsd_heap_current_instance(&noct_heap) == 128);

	zedbsd_heap_reset_instance(&noct_heap);
	assert(zedbsd_heap_current_instance(&noct_heap) == 0);
	assert(memcmp(persistent, "persistent", 11) == 0);
	assert(zedbsd_heap_validate_instance(&kernel_heap));
	assert(zedbsd_heap_validate_instance(&noct_heap));

	assert(zedbsd_heap_set_active(&kernel_heap) == &noct_heap);
	zedbsd_free(persistent);
	assert(zedbsd_heap_current_instance(&kernel_heap) == 0);
	(void)zedbsd_heap_set_active(previous);
	puts("zedBSD heap-context isolation tests: PASS");
	return 0;
}
