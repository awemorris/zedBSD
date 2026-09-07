/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

/* Captures full canonical pages before an executable mapping becomes visible. */
int
file_exec_snapshot_create(
	struct file_content_lease *input,
	off_t offset,
	size_t length,
	struct file_exec_snapshot **result)
{
	struct file_exec_snapshot *snapshot;
	struct vm_object_page *page;
	size_t count, bytes, index;
	int error;

	if (result == NULL)
		return EINVAL;
	*result = NULL;
	if (input == NULL || !input->active || offset < 0 || length == 0 ||
	    ((uint64_t)offset & (ZEDBSD_PAGE_SIZE - 1U)) != 0 ||
	    (length & (ZEDBSD_PAGE_SIZE - 1U)) != 0 || offset > input->size ||
	    (uint64_t)length > (uint64_t)(input->size - offset))
		return EINVAL;
	if (!input->shared_read || input->read_object == NULL)
		return EOPNOTSUPP;
	count = length / ZEDBSD_PAGE_SIZE;
	if (count > (SIZE_MAX - sizeof(*snapshot)) / sizeof(snapshot->pages[0]))
		return EOVERFLOW;
	bytes = sizeof(*snapshot) + count * sizeof(snapshot->pages[0]);
	error = cache_memory_reserve(CACHE_MEMORY_FILE_META, bytes, 1);
	if (error != 0)
		return error;
	snapshot = kern_calloc(1, bytes);
	if (snapshot == NULL) {
		cache_memory_cancel(CACHE_MEMORY_FILE_META, bytes);
		return ENOMEM;
	}
	cache_memory_commit(CACHE_MEMORY_FILE_META, bytes);
	refcount_init(&snapshot->refs, 1);
	snapshot->memory_bytes = bytes;
	snapshot->offset = offset;
	snapshot->length = length;
	error = file_exec_snapshot_begin(input->file, &snapshot->input);
	if (error != 0)
		goto fail;
	if (!snapshot->input.shared_read ||
	    snapshot->input.read_object != input->read_object ||
	    snapshot->input.size != input->size) {
		error = EIO;
		goto fail;
	}

	/* Persistent page pins prevent reclaim from replacing captured source bytes. */
	for (index = 0; index < count; index++) {
		error = vm_object_fault(snapshot->input.read_object,
		    offset + (off_t)(index * ZEDBSD_PAGE_SIZE), &page);
		if (error != 0)
			goto fail;
		error = vm_object_page_pin(page);
		vm_object_fault_release(page);
		if (error != 0)
			goto fail;
		snapshot->pages[snapshot->page_count++] = page;
	}
	*result = snapshot;
	return 0;

fail:
	file_exec_snapshot_put(snapshot);
	return error;
}

/* Shares immutable ownership across VM regions, splits and fork. */
void
file_exec_snapshot_ref(struct file_exec_snapshot *snapshot)
{
	if (snapshot != NULL)
		refcount_get(&snapshot->refs);
}

/* No object mapping put may wait on the loader's still-active cache pin. */
void
file_exec_snapshot_put(struct file_exec_snapshot *snapshot)
{
	size_t index;

	if (snapshot == NULL || !refcount_put(&snapshot->refs))
		return;
	for (index = 0; index < snapshot->page_count; index++)
		vm_object_page_unpin(snapshot->pages[index]);
	file_content_lease_end(&snapshot->input);
	cache_memory_release(CACHE_MEMORY_FILE_META, snapshot->memory_bytes);
	kern_free(snapshot);
}
