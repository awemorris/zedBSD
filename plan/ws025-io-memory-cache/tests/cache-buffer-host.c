/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#define main buffer_regression_main
#include "plan/ws025-io-memory-cache/tests/buffer-run-host.c"
#undef main

bool hal_irq_disable(void) { return false; }
void hal_irq_enable(void) { }
void hal_pmem_get_stats(struct hal_pmem_stats *stats)
{
	memset(stats, 0, sizeof(*stats));
	stats->physical_total = 64U * 1024U * 1024U;
	stats->physical_free = stats->physical_total - (size_t)live_pages * 4096U;
}

int main(void)
{
	struct cache_memory_stats memory;
	struct bufcache_stats buffers;
	struct buf *buffer;
	struct buf *selected;
	unsigned previous_writes;

	cache_memory_init();
	assert(buffer_regression_main() == 0);
	cache_memory_get_stats(&memory);
	buf_get_stats(&buffers);
	assert(memory.pending_bytes == 0);
	assert(memory.resident_bytes == buffers.current_bytes);
	assert(memory.usage[CACHE_MEMORY_BUF_META].resident_bytes == buffers.metadata_bytes);
	assert(memory.usage[CACHE_MEMORY_BUF_DATA].resident_bytes == buffers.data_bytes);
	assert(cache_memory_set_target(4096) == 0);
	assert(buf_get(&device, 0, &buffer) == ENOMEM);
	cache_memory_get_stats(&memory);
	assert(memory.pending_bytes == 0 && memory.resident_bytes == 4096);

	/* A dirty buffer cannot be counted as free or written under clean pressure. */
	assert(cache_memory_set_target(1024U * 1024U) == 0);
	assert(buf_get(&device, 0, &buffer) == 0);
	memset(buffer->b_data, 0x73, buffer->b_size);
	buf_mark_dirty(buffer);
	buf_release(buffer);
	previous_writes = writes;
	assert(cache_memory_set_target(4096) == EBUSY);
	assert(writes == previous_writes);
	cache_memory_get_stats(&memory);
	assert(memory.target_bytes == 1024U * 1024U && !memory.resizing);
	assert(buf_sync(&device) == 0);
	previous_writes = writes;
	assert(cache_memory_set_target(4096) == 0);
	assert(writes == previous_writes);

	/* A referenced buffer prevents shrink until the caller releases ownership. */
	assert(cache_memory_set_target(1024U * 1024U) == 0);
	assert(buf_get(&device, 8, &buffer) == 0);
	assert(cache_memory_set_target(4096) == EBUSY);
	buf_release(buffer);
	assert(cache_memory_set_target(4096) == 0);
	cache_memory_get_stats(&memory);
	assert(memory.pending_bytes == 0 && memory.resident_bytes == 4096);

	/* Dirty membership keeps device identity and selection references independent. */
	assert(cache_memory_set_target(1024U * 1024U) == 0);
	assert(dirty_head == NULL && dirty_tail == NULL);
	assert(device.d_dirty_buffers == NULL && nested_device.d_dirty_buffers == NULL);
	assert(buf_get(&device, 0, &buffer) == 0);
	buf_mark_dirty(buffer);
	buf_release(buffer);
	assert(buf_get(&nested_device, 0, &buffer) == 0);
	buf_mark_dirty(buffer);
	buf_release(buffer);
	assert(device.d_dirty_buffers != NULL && nested_device.d_dirty_buffers != NULL);
	selected = dirty_reference(&device, 0, device.d_block_count, 0);
	assert(selected != NULL && selected->b_disk == &device);
	assert(buf_invalidate_disk(&device, BUF_INVALIDATE_DISCARD) == EBUSY);
	drop_caller_reference(selected);
	fault_at = calls + 1;
	assert(buf_sync(&device) == EIO);
	assert(device.d_dirty_buffers != NULL && nested_device.d_dirty_buffers != NULL);
	fault_at = 0;
	redirty = 1;
	previous_writes = writes;
	assert(buf_sync(&device) == 0);
	assert(writes == previous_writes + 2);
	assert(device.d_dirty_buffers == NULL && nested_device.d_dirty_buffers != NULL);
	assert(buf_invalidate_disk(&nested_device, BUF_INVALIDATE_DISCARD) == 0);
	assert(dirty_head == NULL && dirty_tail == NULL && nested_device.d_dirty_buffers == NULL);
	assert(cache_memory_set_target(4096) == 0);
	puts("dirty buffer index PASS: selected lifetime, device isolation, failed/redirty retry, discard removal");
	puts("shared buffer accounting PASS: real reservations, dirty/pin refusal, no-I/O shrink");
	return 0;
}
