/* Focused allocator and shared-lock-domain regression for ws004-p008. */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libc/heap.h"

#define SLOT_COUNT 256U
#define RANDOM_OPERATIONS 100000U
#define THREAD_COUNT 8U
#define THREAD_OPERATIONS 20000U

struct slot {
	void *pointer;
	size_t size;
	unsigned char pattern;
};

static pthread_mutex_t shared_lock = PTHREAD_MUTEX_INITIALIZER;
static struct heap_allocator concurrent_heap;

static int
aligned_prefix_regression_test(void)
{
	static _Alignas(16) unsigned char storage[4096U];
	struct heap_allocator heap;
	void *pointer;

	heap_allocator_init(&heap, storage, sizeof(storage));
	pointer = heap_allocator_aligned_alloc(&heap, 16U, 64U);
	if (pointer == NULL || (uintptr_t)pointer % 16U != 0 ||
	    !heap_allocator_validate(&heap))
		return 0;
	heap_allocator_free(&heap, pointer);
	return heap_allocator_validate(&heap);
}

struct grow_context {
	unsigned char *limit;
};

static size_t
grow_heap(void *argument, void *end, size_t minimum)
{
	struct grow_context *context = argument;
	size_t available = (size_t)(context->limit - (unsigned char *)end);
	size_t amount;

	if (minimum > SIZE_MAX - 7U)
		return 0;
	amount = (minimum + 7U) & ~(size_t)7U;
	return amount <= available ? amount : 0;
}

static int
grow_regression_test(void)
{
	static _Alignas(16) unsigned char storage[32U * 1024U];
	struct grow_context context = { storage + sizeof(storage) };
	struct heap_allocator heap;
	void *pointer;

	heap_allocator_init(&heap, storage, 1024U);
	heap_allocator_set_grow(&heap, grow_heap, &context);
	pointer = heap_allocator_alloc(&heap, 8192U);
	if (pointer == NULL || !heap_allocator_validate(&heap))
		return 0;
	memset(pointer, 0xa5, 8192U);
	heap_allocator_free(&heap, pointer);
	return heap_allocator_validate(&heap) &&
	    heap_allocator_current(&heap) == 0;
}

/* Strong test hooks model the kernel override of libc's weak no-op hooks. */
void
__libc_heap_lock(void)
{
	if (pthread_mutex_lock(&shared_lock) != 0)
		abort();
}

void
__libc_heap_unlock(void)
{
	if (pthread_mutex_unlock(&shared_lock) != 0)
		abort();
}

static uint32_t
random_next(uint32_t *state)
{
	uint32_t value = *state;

	value ^= value << 13;
	value ^= value >> 17;
	value ^= value << 5;
	*state = value;
	return value;
}

static int
check_slot(const struct slot *slot)
{
	const unsigned char *bytes = slot->pointer;
	size_t index;

	for (index = 0; index < slot->size; index++)
		if (bytes[index] != slot->pattern)
			return 0;
	return 1;
}

static int
randomized_allocator_test(void)
{
	static unsigned char storage[2U * 1024U * 1024U];
	struct heap_allocator heap;
	struct slot slots[SLOT_COUNT] = { 0 };
	uint32_t random = 0x91e10da5U;
	unsigned operation;

	heap_allocator_init(&heap, storage, sizeof(storage));
	if (!heap_allocator_validate(&heap))
		return 0;
	for (operation = 0; operation < RANDOM_OPERATIONS; operation++) {
		unsigned index = random_next(&random) % SLOT_COUNT;
		struct slot *slot = &slots[index];
		unsigned action = random_next(&random) % 4U;

#ifdef HEAP_TEST_TRACE
	fprintf(stderr, "operation=%u slot=%u action=%u pointer=%p size=%zu\n",
	    operation, index, action, slot->pointer, slot->size);
#endif

		if (slot->pointer != NULL && !check_slot(slot))
			return 0;
		if (slot->pointer == NULL) {
			size_t size = (random_next(&random) % 4096U) + 1U;
			unsigned char pattern = (unsigned char)random_next(&random);
			void *pointer;

			if (action == 0) {
				size_t alignment = (size_t)1U <<
				    (4U + random_next(&random) % 8U);
				pointer = heap_allocator_aligned_alloc(&heap,
				    alignment, size);
#ifdef HEAP_TEST_TRACE
				fprintf(stderr,
				    "  aligned allocation size=%zu alignment=%zu result=%p\n",
				    size, alignment, pointer);
#endif
				if (pointer != NULL &&
				    (uintptr_t)pointer % alignment != 0)
					return 0;
			} else {
				pointer = heap_allocator_alloc(&heap, size);
#ifdef HEAP_TEST_TRACE
				fprintf(stderr, "  allocation size=%zu result=%p\n",
				    size, pointer);
#endif
			}
			if (pointer != NULL) {
				memset(pointer, pattern, size);
				slot->pointer = pointer;
				slot->size = size;
				slot->pattern = pattern;
			}
		} else if (action < 2U) {
			heap_allocator_free(&heap, slot->pointer);
			memset(slot, 0, sizeof(*slot));
		} else {
			size_t size = (random_next(&random) % 4096U) + 1U;
			void *replacement = heap_allocator_realloc(&heap,
			    slot->pointer, size);

			if (replacement != NULL) {
				size_t preserved = size < slot->size ? size : slot->size;
				size_t offset;

				for (offset = 0; offset < preserved; offset++)
					if (((unsigned char *)replacement)[offset] !=
					    slot->pattern)
						return 0;
				slot->pattern = (unsigned char)random_next(&random);
				memset(replacement, slot->pattern, size);
				slot->pointer = replacement;
				slot->size = size;
			}
		}
		if (
#ifdef HEAP_TEST_TRACE
		    1 &&
#else
		    (operation & 127U) == 0 &&
#endif
		    !heap_allocator_validate(&heap))
			return 0;
	}
	for (operation = 0; operation < SLOT_COUNT; operation++)
		if (slots[operation].pointer != NULL) {
			if (!check_slot(&slots[operation]))
				return 0;
			heap_allocator_free(&heap, slots[operation].pointer);
		}
	return heap_allocator_validate(&heap) &&
	    heap_allocator_current(&heap) == 0;
}

struct worker_context {
	unsigned index;
};

static void *
concurrent_worker(void *argument)
{
	struct worker_context *context = argument;
	uint32_t random = 0x1234567U ^ (context->index * 0x9e3779b9U);
	unsigned operation;

	for (operation = 0; operation < THREAD_OPERATIONS; operation++) {
		size_t size = (random_next(&random) % 512U) + 1U;
		void *pointer;

		if ((context->index & 1U) == 0) {
			pointer = heap_alloc_active(size);
			if (pointer != NULL) {
				memset(pointer, (int)context->index, size);
				heap_free_active(pointer);
			}
		} else {
			if (pthread_mutex_lock(&shared_lock) != 0)
				return (void *)1;
			pointer = heap_allocator_alloc(&concurrent_heap, size);
			if (pointer != NULL) {
				memset(pointer, (int)context->index, size);
				heap_allocator_free(&concurrent_heap, pointer);
			}
			if (pthread_mutex_unlock(&shared_lock) != 0)
				return (void *)1;
		}
	}
	return NULL;
}

static int
shared_lock_domain_test(void)
{
	static unsigned char storage[1024U * 1024U];
	struct heap_allocator *previous;
	struct worker_context contexts[THREAD_COUNT];
	pthread_t threads[THREAD_COUNT];
	unsigned index;

	heap_allocator_init(&concurrent_heap, storage, sizeof(storage));
	previous = heap_active_set(&concurrent_heap);
	for (index = 0; index < THREAD_COUNT; index++) {
		contexts[index].index = index;
		if (pthread_create(&threads[index], NULL, concurrent_worker,
		    &contexts[index]) != 0)
			return 0;
	}
	for (index = 0; index < THREAD_COUNT; index++) {
		void *result = NULL;

		if (pthread_join(threads[index], &result) != 0 || result != NULL)
			return 0;
	}
	heap_active_set(previous);
	return heap_allocator_validate(&concurrent_heap) &&
	    heap_allocator_current(&concurrent_heap) == 0;
}

static int
invalid_free_test(void)
{
	static unsigned char storage[4096U];
	struct heap_allocator heap;
	void *pointer;
	size_t errors;

	heap_allocator_init(&heap, storage, sizeof(storage));
	pointer = heap_allocator_alloc(&heap, 64U);
	if (pointer == NULL)
		return 0;
	errors = heap_allocator_error_count(&heap);
	heap_allocator_free(&heap, (unsigned char *)pointer + 1U);
	if (heap_allocator_error_count(&heap) != errors + 1U ||
	    !heap_allocator_validate(&heap))
		return 0;
	heap_allocator_free(&heap, pointer);
	return heap_allocator_validate(&heap);
}

int
main(void)
{
	if (!aligned_prefix_regression_test()) {
		fputs("aligned-prefix allocator regression failed\n", stderr);
		return EXIT_FAILURE;
	}
	if (!grow_regression_test()) {
		fputs("heap-grow allocator regression failed\n", stderr);
		return EXIT_FAILURE;
	}
	if (!randomized_allocator_test()) {
		fputs("randomized allocator invariant test failed\n", stderr);
		return EXIT_FAILURE;
	}
	if (!shared_lock_domain_test()) {
		fputs("shared heap lock-domain test failed\n", stderr);
		return EXIT_FAILURE;
	}
	if (!invalid_free_test()) {
		fputs("invalid-free allocator test failed\n", stderr);
		return EXIT_FAILURE;
	}
	puts("kernel heap lock/invariant model: PASS");
	return EXIT_SUCCESS;
}
