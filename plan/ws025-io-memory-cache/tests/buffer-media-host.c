/* Actual cache retirement retains active users and never writes old media. */
#define main retained_buffer_main
#include "buffer-run-host.c"
#undef main

int main(void)
{
	struct buf *buffer;
	unsigned old_writes;

	assert(retained_buffer_main() == 0);
	assert(buf_discard_media(NULL) == EINVAL);
	assert(buf_discard_media(&device) == EINVAL);
	assert(buf_get(&device, 0, &buffer) == 0);
	memset(buffer->b_data, 0xa7, buffer->b_size);
	buf_mark_dirty(buffer);
	old_writes = writes;
	atomic_raw_store_release(&device.d_media_revoked, 1U);
	assert(buf_discard_media(&device) == EBUSY);
	assert(writes == old_writes && (buffer->b_flags & BUF_DIRTY));
	buf_release(buffer);
	assert(buf_discard_media(&device) == 0);
	assert(writes == old_writes && cache_dirty_bytes == 0 && stat_buffers == 0);
	assert(device.d_dirty_buffers == NULL && live_pages == 1);
	assert(buf_discard_media(&device) == 0);
	assert(device.d_buffer_refs == 0);
	assert(buf_get(&device, 8, &buffer) == ENXIO);
	assert(device.d_buffer_refs == 0 && live_pages == 1);
	puts("revoked buffer retirement PASS: pinned refusal, dirty discard, no backend write, idempotence");
	return 0;
}
