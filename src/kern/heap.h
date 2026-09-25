/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The fixed kernel heap allocator.
 *
 * This is the block allocator behind kern_malloc() for small requests.  It is
 * an implementation detail of src/kern/entry.c, which owns the one heap
 * instance, its storage and its lock; the rest of the kernel allocates through
 * <kern/kmem.h>.  The allocator takes no lock of its own.
 */

#ifndef KERN_SRC_KERN_HEAP_H
#define KERN_SRC_KERN_HEAP_H

#include <stddef.h>
#include <stdint.h>

/*
 * What an observer is told about.
 */
enum kern_heap_event {
	KERN_HEAP_EVENT_ALLOCATED = 0,
	KERN_HEAP_EVENT_FREED
};

/*
 * A function told about every allocation and release of a heap.
 */
typedef void (*kern_heap_observer_fn)(void *context, void *pointer, size_t size, enum kern_heap_event event);

struct kern_heap_block;

/*
 * One heap: a fixed range of memory carved into blocks.
 *
 * Every block of the range is on the physical chain in address order, and the
 * free ones are also on the free list.  The owner initializes the heap once
 * over its storage and serializes every call.  errors counts the releases of
 * pointers the heap does not own; the owner reads it to detect corruption.
 */
struct kern_heap {
	uint8_t *begin;
	uint8_t *end;
	struct kern_heap_block *first;
	struct kern_heap_block *free_list;
	size_t current_bytes;
	size_t peak_bytes;
	size_t largest_failed_allocation;
	size_t errors;
	kern_heap_observer_fn observer;
	void *observer_context;
};

void
kern_heap_init(
	struct kern_heap *heap,
	void *base,
	size_t size);

void *
kern_heap_alloc(
	struct kern_heap *heap,
	size_t size);

void
kern_heap_free(
	struct kern_heap *heap,
	void *pointer);

size_t
kern_heap_current(
	const struct kern_heap *heap);

size_t
kern_heap_peak(
	const struct kern_heap *heap);

size_t
kern_heap_largest_failed(
	const struct kern_heap *heap);

size_t
kern_heap_largest_free(
	const struct kern_heap *heap);

int
kern_heap_validate(
	const struct kern_heap *heap);

#ifdef KERN_KERNEL_HEAP_TRACE
void
kern_heap_set_observer(
	struct kern_heap *heap,
	kern_heap_observer_fn observer,
	void *context);

int
kern_heap_trace_validate(
	const struct kern_heap *heap);

/*
 * Records a pointer about to be released.
 *
 * The kernel heap trace build defines it (src/kern/entry.c) to write the
 * pointer and the caller into its ring without allocating.
 */
void
kern_heap_trace_pointer_walk(
	void *pointer,
	void *caller);
#endif

#endif
