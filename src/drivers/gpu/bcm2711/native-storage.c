/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Native upload owners separate logical callback completion, DMA retirement and final translation flush. */
#include <kern/dcache.h>
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"
#include "drivers/gpu/bcm2711/native-storage.h"

/* The underlying contiguous owner accepts at most 256 MiB per physical allocation. */
#define NATIVE_STORAGE_BYTES (256ULL * 1024U * 1024U)

/*
 * Allocates and maps one complete native upload interval before giving it to a prepared job owner.
 *
 * A failed translation publication remains owned by the native space's
 * existing quarantine.  The caller must close native admission after that
 * failure and use checked recovery rather than repeat publication blindly.
 */
int
bcm2711_native_storage_create(
	struct bcm2711_v3d_space *space,
	uint64_t bytes,
	struct bcm2711_native_storage **storage)
{
	struct bcm2711_native_storage *created;
	struct bcm2711_buffer *buffer;
	struct bcm2711_v3d_view *view;
	uint64_t limit;
	uint32_t physical_bits;
	int error;

	/* Every failure leaves the caller without a partially initialized upload owner. */
	if (storage == NULL)
		return EINVAL;
	*storage = NULL;

	/* Only the actual initialized native address space can supply identified placement constraints. */
	if (space == NULL || space->native == NULL)
		return EINVAL;

	/* Per-allocation bounds also keep complete clean/copy operations representable on this architecture. */
	if (bytes == 0 || bytes > NATIVE_STORAGE_BYTES)
		return EINVAL;

	/* Placement uses actual physical addressing capacity, without a guessed bus alias or framebuffer limit. */
	physical_bits = space->native->hardware.physical_bits;
	if (physical_bits < 30U || physical_bits > 36U)
		return ENOTSUP;
	limit = ((uint64_t)1 << physical_bits) - 1U;

	/* The CPU owner exists before any physical run or native translation can be acquired. */
	created = kern_calloc(1, sizeof(*created));
	if (created == NULL)
		return ENOMEM;

	/* Cached private storage has no exported user alias and is explicitly cleaned before device reads. */
	error = bcm2711_buffer_create(bytes, limit, 4096U, &buffer);
	if (error != 0) {
		kern_free(created);
		return error;
	}

	/* Mapping acquires its own allocation hold before the temporary source reference is retired. */
	error = bcm2711_v3d_memory_map(space, buffer, &view);
	bcm2711_buffer_release(buffer);
	if (error != 0) {
		kern_free(created);
		return error;
	}

	/* Only a complete owned mapping can become native upload storage. */
	created->space = space;
	created->view = view;
	created->bytes = bytes;
	*storage = created;

	/* Succeeded: the caller owns both the VA reservation and the zeroed physical run independently of public resources. */
	return 0;
}

/*
 * Cleans a complete cached upload allocation, including initialized page padding, before native publication.
 */
int
bcm2711_native_storage_clean(
	struct bcm2711_native_storage *storage)
{
	struct bcm2711_v3d_view *view;
	struct bcm2711_buffer *buffer;

	/* An absent or partially retired owner grants no usable native input. */
	if (storage == NULL || storage->view == NULL)
		return EINVAL;
	view = storage->view;

	/* Quarantined or dead translations cannot be cleaned back into executable ownership. */
	if (view->quarantined || view->references == 0)
		return EIO;

	/* Actual cached backing must cover the entire declared upload interval. */
	buffer = view->buffer;
	if (buffer == NULL || buffer->address == NULL)
		return EINVAL;

	/* This private owner never treats non-cacheable exported storage as a cached alias. */
	if (buffer->uncached)
		return EINVAL;

	/* Padding and logical bytes belong to the same whole mapped allocation. */
	if (storage->bytes == 0 ||
	    storage->bytes > buffer->bytes ||
	    buffer->bytes > buffer->memory.size ||
	    buffer->memory.size != view->bytes)
		return EINVAL;

	/* Cache maintenance completes CPU writes before the caller's later command publication barrier. */
	kern_dcache_clean_range(buffer->address, (size_t)buffer->memory.size);

	/* Succeeded: device reads can consume the immutable upload bytes without a dirty CPU cache alias. */
	return 0;
}

/*
 * Uploads the exact independently compiled instruction words into owned, aligned native code storage.
 */
int
bcm2711_native_program_upload(
	struct bcm2711_v3d_space *space,
	const struct bcm2711_shader_binary *binary,
	struct bcm2711_native_storage **storage)
{
	struct bcm2711_native_storage *created;
	uint8_t *destination;
	uint64_t bytes;
	uint64_t instruction;
	uint32_t index;
	uint32_t byte;
	int error;
	int released;

	/* A failed upload never publishes a half-filled native program to the caller. */
	if (storage == NULL)
		return EINVAL;
	*storage = NULL;

	/* Compiler products retain real immutable instruction arrays through their pipeline owners. */
	if (binary == NULL ||
	    binary->code == NULL ||
	    binary->code_count == 0)
		return EINVAL;

	/* Whole instruction byte counts are measured before allocation or any array access. */
	bytes = (uint64_t)binary->code_count * 8U;
	if (bytes > NATIVE_STORAGE_BYTES)
		return ENOTSUP;

	/* One native mapping owns the complete code stream through later execution or checked retirement. */
	error = bcm2711_native_storage_create(space, bytes, &created);
	if (error != 0)
		return error;

	/* Numerical QPU words are stored explicitly in little-endian hardware order, independent of the host fixture. */
	destination = created->view->buffer->address;
	for (index = 0; index < binary->code_count; index++) {
		/* Every instruction keeps all eight native bytes without an unaligned CPU word store. */
		instruction = binary->code[index];
		for (byte = 0; byte < 8; byte++)
			destination[(size_t)index * 8U + byte] = (uint8_t)(instruction >> (byte * 8U));
	}

	/* All compiled bytes and zero allocation padding become visible before code addresses can enter a native record. */
	error = bcm2711_native_storage_clean(created);
	if (error != 0) {
		released = bcm2711_native_storage_release(space, &created, true);
		if (released != 0)
			return released;
		return error;
	}

	/* Publication transfers the complete independent native code owner to its prepared job. */
	*storage = created;

	/* Succeeded: subsequent pipeline identity withdrawal cannot invalidate this job-owned uploaded instruction interval. */
	return 0;
}

/*
 * Consumes one upload owner only after native DMA retirement, preserving failed translation teardown in the space quarantine.
 */
int
bcm2711_native_storage_release(
	struct bcm2711_v3d_space *space,
	struct bcm2711_native_storage **storage,
	bool retired)
{
	struct bcm2711_native_storage *owned;
	int error;

	/* A missing owner pointer cannot participate in an explicit consumption contract. */
	if (storage == NULL)
		return EINVAL;

	/* Already consumed owners require no further translation or physical retirement. */
	owned = *storage;
	if (owned == NULL)
		return 0;

	/* Logical callback completion alone never supplies proof that DMA stopped using this code or scratch interval. */
	if (!retired)
		return EBUSY;

	/* A malformed retirement context cannot consume an otherwise valid independent native owner. */
	if (space == NULL ||
	    space->native == NULL ||
	    owned->view == NULL)
		return EINVAL;

	/* A different controller cannot withdraw this owner's actual native VA reservation. */
	if (owned->space != space)
		return EINVAL;

	/* The native space owns a failed final unmap/flush; callers must never retry a consumed mapped reference. */
	*storage = NULL;
	error = bcm2711_v3d_memory_release(space, owned->view);
	kern_free(owned);
	if (error != 0)
		return error;

	/* Succeeded: both the independent upload owner and its fully flushed native mapping retired. */
	return 0;
}
