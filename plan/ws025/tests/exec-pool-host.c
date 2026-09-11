/* Production copy_segment_snapshot plus the production shared pool owner. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <hal/hal.h>
#include <kern/io-pool.h>
#include <kern/io-stats.h>
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "io-pool-hal-host.h"

#define ELF_OFF_MAX INT64_MAX
struct file_content_lease { int held; };
struct vmspace { unsigned char image[262144]; };
static unsigned char content[262144];
static unsigned reads, copies, fail_read, short_read, fail_copy;
static size_t expected_chunk;
static int large_source;

static ssize_t file_content_lease_pread(struct file_content_lease *lease, void *buffer, size_t size, off_t offset)
{
	assert(lease->held && size <= expected_chunk);
	reads++;
	if (reads == fail_read) return -EIO;
	if (reads == short_read) return 0;
	if (large_source) { memset(buffer, 0x73, size); return (ssize_t)size; }
	assert(offset >= 0 && (size_t)offset + size <= sizeof(content));
	memcpy(buffer, content + offset, size);
	return (ssize_t)size;
}
static int vmspace_copy_to(struct vmspace *vm, uintptr_t destination, const void *buffer, size_t size)
{
	copies++;
	if (copies == fail_copy) return EFAULT;
	assert(destination + size <= sizeof(vm->image));
	memcpy(vm->image + destination, buffer, size);
	return 0;
}
#include "exec-copy-extracted.h"

int main(void)
{
	struct file_content_lease lease = {1};
	static struct vmspace vm;
	struct io_pool_stats stats;
	void *held[320];
	unsigned count, index, mode, failure, saved;
	size_t capacity;
	size_t length = 131195;
	int result;

	io_pool_init();
	io_pool_get_stats(&stats);
	saved = allocation_calls;
	for (index = 0; index < sizeof(content); index++) content[index] = (unsigned char)(index * 7 + 3);
	count = 0;
	for (mode = 0; mode < 3; mode++) {
		if (mode != 0) {
			unsigned target = mode == 1 ? stats.large_count : stats.large_count + stats.small_count;
			while (count < target) { capacity = 0; held[count] = io_pool_borrow(KERN_IO_BATCH_MAX, &capacity); assert(held[count]); count++; }
		}
		expected_chunk = mode == 0 ? KERN_IO_BATCH_MAX : mode == 1 ? KERN_IO_SMALL_SIZE : 512;
		for (failure = 0; failure < 4; failure++) {
			memset(vm.image, 0, sizeof(vm.image));
			reads = copies = 0;
			fail_read = failure == 1 ? 2 : 0;
			short_read = failure == 2 ? 2 : 0;
			fail_copy = failure == 3 ? 2 : 0;
			result = copy_segment_snapshot(&lease, &vm, 123, 17, length);
			assert(result == (failure == 0 ? 0 : failure == 3 ? EFAULT : EIO));
			assert(vm.image[122] == 0 && vm.image[123 + length] == 0);
			if (failure == 0) {
				assert(memcmp(vm.image + 123, content + 17, length) == 0);
				assert(reads == (length + expected_chunk - 1) / expected_chunk);
			}
			io_pool_get_stats(&stats);
			assert(stats.in_use == count && allocation_calls == saved && lease.held);
		}
	}
	for (index = 0; index < count; index++) io_pool_release(held[index]);
	io_pool_get_stats(&stats);
	assert(stats.in_use == 0);
	assert(copy_segment_snapshot(&lease, &vm, 0, 0, 0) == 0);
	reads = copies = 0; fail_read = short_read = fail_copy = 0;
	assert(copy_segment_snapshot(&lease, &vm, 0, (off_t)INT64_MAX, 0) == 0);
	large_source = 1; expected_chunk = KERN_IO_BATCH_MAX;
	assert(copy_segment_snapshot(&lease, &vm, 0, (off_t)INT64_MAX - 10, length) == EOVERFLOW);
	io_pool_get_stats(&stats); assert(stats.in_use == 0 && allocation_calls == saved);
	puts("PASS: exec lease copy with 64 KiB/4 KiB/stack scratch, short/error/copy cleanup and untouched segment edges");
	return 0;
}
