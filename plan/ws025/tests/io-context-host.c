/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <kern/io-context.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
	struct io_context root, loop, metadata, saved;
	unsigned i;
	assert(io_context_child(&root, NULL, 0) == 0);
	root.origin_inode = (struct inode *)(uintptr_t)0x1234;
	root.claim = (const struct backing_claim *)(uintptr_t)0x5678;
	root.content_generation = UINT64_MAX - 1;
	root.media_generation = 71;
	assert(io_context_child(&loop, &root, IO_CONTEXT_DRAIN) == 0);
	assert(loop.origin_inode == root.origin_inode && loop.claim == root.claim);
	assert(loop.content_generation == root.content_generation && loop.media_generation == 71);
	assert(loop.flags == (IO_CONTEXT_THROUGH | IO_CONTEXT_DRAIN) && loop.depth == 1);
	assert(io_context_child(&metadata, &loop, IO_CONTEXT_ORDERED) == 0);
	assert(metadata.flags == (IO_CONTEXT_THROUGH | IO_CONTEXT_DRAIN | IO_CONTEXT_ORDERED));
	saved = metadata;
	assert(io_context_child(&metadata, &loop, 0x80) == EOPNOTSUPP);
	assert(memcmp(&saved, &metadata, sizeof(saved)) == 0);
	for (i = 0; i < IO_CONTEXT_DEPTH_MAX; i++)
		assert(io_context_child(&root, &root, 0) == 0);
	saved = root;
	assert(io_context_child(&root, &root, 0) == ELOOP);
	assert(memcmp(&saved, &root, sizeof(saved)) == 0);
	root.flags = 0x80;
	assert(io_context_validate(&root) == EOPNOTSUPP);
	puts("I/O context PASS: provenance, monotonic drain/ordered flags, unsupported refusal, bounded depth");
	return 0;
}
