/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
extern void *io_pool_borrow(size_t, size_t *) __attribute__((weak));
extern void io_pool_release(void *) __attribute__((weak));

/* Keeps normal BIO admission, sequencing, claims and error-prefix semantics. */
int
disk_transfer_vector_context(
	struct disk *disk,
	enum bio_op op,
	uint64_t block,
	const struct disk_vector *vectors,
	unsigned count,
	uint32_t *completed,
	const struct io_context *context)
{
	void *scratch;
	size_t total, capacity, offset, length, confirmed;
	uint32_t blocks, progress;
	unsigned index;
	int contiguous, error;

	if (completed == NULL)
		return EINVAL;
	*completed = 0;
	if (disk == NULL || vectors == NULL || count == 0 || count > DISK_VECTOR_MAX ||
	    (op != BIO_READ && op != BIO_WRITE) || disk->d_block_size == 0)
		return EINVAL;
	total = 0;
	contiguous = 1;
	for (index = 0; index < count; index++) {
		length = vectors[index].length;
		if (vectors[index].data == NULL || length == 0 ||
		    length % disk->d_block_size != 0 || length > KERN_IO_BATCH_MAX - total ||
		    length > UINTPTR_MAX - (uintptr_t)vectors[index].data)
			return EINVAL;
		if (index != 0 && (uintptr_t)vectors[index - 1U].data +
		    vectors[index - 1U].length != (uintptr_t)vectors[index].data)
			contiguous = 0;
		total += length;
	}
	blocks = (uint32_t)(total / disk->d_block_size);
	if (block >= disk->d_block_count || blocks > disk->d_block_count - block)
		return EINVAL;
	if (contiguous) {
		io_stats_record(IO_DISK_VECTOR_BATCH, total);
		return disk_transfer_progress_context(disk, op, block, blocks,
		    vectors[0].data, completed, context);
	}

	/* Normal admission uses one existing bounded scratch run, never an allocation. */
	scratch = NULL;
	capacity = 0;
	if (io_pool_borrow != NULL && io_pool_release != NULL)
		scratch = io_pool_borrow(total, &capacity);
	if (scratch != NULL && capacity < total) {
		io_pool_release(scratch);
		scratch = NULL;
	}
	if (scratch != NULL) {
		if (op == BIO_WRITE) {
			offset = 0;
			for (index = 0; index < count; index++) {
				memcpy((char *)scratch + offset, vectors[index].data, vectors[index].length);
				offset += vectors[index].length;
			}
		}
		io_stats_record(IO_DISK_VECTOR_BATCH, total);
		error = disk_transfer_progress_context(disk, op, block, blocks,
		    scratch, completed, context);
		if (*completed > blocks)
			HAL_FATAL("disk vector confirmed prefix exceeds request");
		confirmed = (size_t)*completed * disk->d_block_size;
		if (op == BIO_READ) {
			offset = 0;
			for (index = 0; index < count && offset < confirmed; index++) {
				length = vectors[index].length;
				if (length > confirmed - offset)
					length = confirmed - offset;
				memcpy(vectors[index].data, (char *)scratch + offset, length);
				offset += length;
			}
		}
		io_pool_release(scratch);
		return error;
	}

	/* Pool exhaustion retains progress through validated caller-owned spans. */
	io_stats_record(IO_DISK_VECTOR_SPLIT, total);
	for (index = 0; index < count; index++) {
		blocks = (uint32_t)(vectors[index].length / disk->d_block_size);
		error = disk_transfer_progress_context(disk, op, block + *completed,
		    blocks, vectors[index].data, &progress, context);
		if (progress > blocks)
			HAL_FATAL("disk vector split prefix exceeds request");
		*completed += progress;
		if (error != 0)
			return error;
		if (progress != blocks)
			return EIO;
	}
	return 0;
}
