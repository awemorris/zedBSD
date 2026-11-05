/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Every allocation has one contiguous run, a stable CPU view and checked owners. */
#include <kern/clock.h>
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <kern/klog.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/buffer.h"

/* Diagnostic sums of final buffer releases since the last report, updated atomically by any releasing thread. */
static uint64_t release_count;
static uint64_t release_bytes;
static uint64_t release_ticks;

/*
 * Diagnostic sums of buffer creations since the last report (the alloc cost's
 * parts): the runs made, their bytes, the physical allocator's ticks, and the
 * ticks of zeroing them; and the reused runs the native storage zeroed again
 * (bcm2711_buffer_zeroed).  Updated atomically by any creating thread.
 */
static uint64_t create_count;
static uint64_t create_bytes;
static uint64_t create_pmem_ticks;
static uint64_t create_zero_ticks;
static uint64_t reuse_count;
static uint64_t reuse_bytes;
static uint64_t reuse_zero_ticks;

/* The reused runs given again without zeroing (their users write every byte the device reads). */
static uint64_t kept_count;
static uint64_t kept_bytes;

/* Bounds storage descriptions independently of available physical memory. */
#define BUFFER_MAX_BYTES (256ULL * 1024U * 1024U)

/* The last identity handed to a run; allocation is serialized by each device's mutex, and the counter only grows. */
static uint64_t buffer_identities;

/* Architectures without Normal non-cacheable RAM cannot satisfy coherent allocations. */
extern int kern_pmem_map_uncached(const struct kern_pmem *run, void **mapped) __attribute__((weak));
extern int kern_pmem_unmap_uncached(void *mapped, size_t size) __attribute__((weak));

/*
 * Allocates a page-rounded contiguous run satisfying a device's address limit.
 * Failures leave no allocation owned by the caller.
 */
int
bcm2711_buffer_create(
	uint64_t bytes,
	uint64_t limit,
	size_t alignment,
	struct bcm2711_buffer **result)
{
	struct bcm2711_buffer *buffer;
	uint64_t started;
	size_t rounded;
	int error;
	int released;

	/* Keeps byte rounding and physical placement representable. */
	*result = NULL;
	if (bytes == 0 || bytes > BUFFER_MAX_BYTES)
		return EINVAL;
	if (alignment < 4096U || (alignment & (alignment - 1U)) != 0)
		return EINVAL;
	rounded = (size_t)((bytes + 4095U) & ~4095ULL);

	/* Creates the owner before acquiring its physical allocation. */
	buffer = kern_calloc(1, sizeof(*buffer));
	if (buffer == NULL)
		return ENOMEM;

	/* Allocates under the native placement contract, without a bus-address alias. */
	started = clock_ticks();
	error = kern_pmem_alloc_limited(rounded, alignment, limit, 0, &buffer->memory);
	__atomic_add_fetch(&create_pmem_ticks, clock_ticks() - started, __ATOMIC_RELAXED);
	if (error != 0) {
		kern_free(buffer);
		return error;
	}

	/* Verifies the actual run rather than trusting requested placement flags. */
	if (buffer->memory.size != rounded ||
	    (buffer->memory.paddr & (alignment - 1U)) != 0 ||
	    buffer->memory.paddr > UINT64_MAX - (rounded - 1U) ||
	    (limit != 0 &&
	     (buffer->memory.paddr > limit ||
	      rounded - 1U > limit - buffer->memory.paddr))) {
		/* No device has seen this malformed placement, so physical retirement is safe. */
		released = kern_pmem_free(&buffer->memory);
		if (released != 0) {
			kern_logf("bcm2711: retained invalid placement (%d)\n", released);
			return ENOTSUP;
		}

		/* The descriptor no longer owns physical storage. */
		kern_free(buffer);
		return ENOTSUP;
	}

	/* A successful allocation must supply a usable CPU view of the whole run. */
	buffer->address = kern_pmem_to_kernel(buffer->memory.paddr);
	if (buffer->address == NULL) {
		released = kern_pmem_free(&buffer->memory);
		if (released != 0) {
			/* Preserves the descriptor of a run the allocator refused to retire. */
			kern_logf("bcm2711: unmapped allocation release failed %d\n", released);
			return EFAULT;
		}

		/* The allocator retired the failed CPU view without any DMA publication. */
		kern_free(buffer);
		return EFAULT;
	}

	/* Initializes the bytes before sharing this immutable placement with another owner. */
	started = clock_ticks();
	kern_memset(buffer->address, 0, rounded);
	__atomic_add_fetch(&create_zero_ticks, clock_ticks() - started, __ATOMIC_RELAXED);
	__atomic_add_fetch(&create_count, 1U, __ATOMIC_RELAXED);
	__atomic_add_fetch(&create_bytes, (uint64_t)rounded, __ATOMIC_RELAXED);
	spin_init(&buffer->guard, LOCK_RANK_DEVICE, "bcm2711-buffer");
	buffer->bytes = bytes;
	buffer->references = 1;
	buffer->identity = __atomic_add_fetch(&buffer_identities, 1U, __ATOMIC_RELAXED);
	*result = buffer;

	/* Succeeded: the caller holds the initial reference to zeroed contiguous RAM. */
	return 0;
}

/*
 * Allocates one Normal non-cacheable run without retaining a cached CPU alias.
 */
int
bcm2711_buffer_create_uncached(
	uint64_t bytes,
	uint64_t limit,
	size_t alignment,
	struct bcm2711_buffer **result)
{
	struct bcm2711_buffer *buffer;
	void *mapped;
	int error;

	/* Both alias lifetime operations must exist before any physical allocation is acquired. */
	*result = NULL;
	if (kern_pmem_map_uncached == NULL || kern_pmem_unmap_uncached == NULL)
		return ENOTSUP;

	/* The initial unpublished owner supplies checked placement and zeroed bytes. */
	error = bcm2711_buffer_create(bytes, limit, alignment, &buffer);
	if (error != 0)
		return error;

	/* Mapping cleans and discards the former direct-map cache lines before publishing Normal NC translations. */
	mapped = NULL;
	error = kern_pmem_map_uncached(&buffer->memory, &mapped);
	if (error != 0) {
		bcm2711_buffer_release(buffer);
		return error;
	}

	/* No later CPU owner reads or writes the allocation's former cached direct-map address. */
	buffer->address = mapped;
	buffer->uncached = true;
	*result = buffer;

	/* Succeeded: all retained CPU owners use the same Normal non-cacheable RAM alias. */
	return 0;
}

/*
 * Retains storage protected by an existing reference or a publisher's mutex.
 */
void
bcm2711_buffer_retain(
	struct bcm2711_buffer *buffer)
{
	unsigned long enabled;

	/* Detects reference misuse before creating an unbounded lifetime or resurrection. */
	enabled = spin_lock_irqsave(&buffer->guard);

	if (buffer->references == 0 || buffer->references == UINT64_MAX)
		__builtin_trap();
	buffer->references++;

	spin_unlock_irqrestore(&buffer->guard, enabled);

	/* Succeeded: the physical allocation has another independently protected owner. */
	return;
}

/*
 * Releases one reference after its owner has stopped every DMA use.
 * A failed physical release keeps the run and descriptor allocated.
 */
void
bcm2711_buffer_release(
	struct bcm2711_buffer *buffer)
{
	unsigned long enabled;
	uint64_t started;
	uint64_t bytes;
	uint64_t remaining;
	int error;

	/* Drops exactly one live reference while another device may retain the allocation. */
	enabled = spin_lock_irqsave(&buffer->guard);

	if (buffer->references == 0)
		__builtin_trap();
	buffer->references--;
	remaining = buffer->references;

	spin_unlock_irqrestore(&buffer->guard, enabled);

	/* Other resource or DMA owners still need the same CPU and physical addresses. */
	if (remaining != 0)
		return;

	/* Diagnostic: the time the uncached unmap and the physical free take. */
	started = clock_ticks();
	bytes = buffer->memory.size;

	/* Every native, resource and VM owner has retired before its uncached translation can be removed. */
	if (buffer->uncached) {
		error = kern_pmem_unmap_uncached(buffer->address, buffer->memory.size);
		if (error != 0) {
			kern_logf("bcm2711: retained failed uncached release %d\n", error);
			return;
		}

		/* A failed later physical release retains RAM without exposing a stale CPU alias. */
		buffer->uncached = false;
		buffer->address = NULL;
	}

	/* Only the final stopped owner may return the physical run to the allocator. */
	error = kern_pmem_free(&buffer->memory);
	if (error != 0) {
		kern_logf("bcm2711: retained failed physical release %d\n", error);
		return;
	}

	/* Diagnostic sums for the GPU's periodic report. */
	(void)__atomic_fetch_add(&release_count, 1U, __ATOMIC_RELAXED);
	(void)__atomic_fetch_add(&release_bytes, bytes, __ATOMIC_RELAXED);
	(void)__atomic_fetch_add(&release_ticks, clock_ticks() - started, __ATOMIC_RELAXED);

	/* No shared reference or native access remains. */
	kern_free(buffer);

	/* Succeeded: the last retired allocation no longer owns CPU or physical storage. */
	return;
}

/*
 * Counts a reused run zeroed again before its next owner (native storage's
 * idle views), for the alloc cost's parts.
 */
void
bcm2711_buffer_zeroed(
	uint64_t bytes,
	uint64_t ticks)
{
	/* One more reuse, its bytes and its zeroing. */
	__atomic_add_fetch(&reuse_count, 1U, __ATOMIC_RELAXED);
	__atomic_add_fetch(&reuse_bytes, bytes, __ATOMIC_RELAXED);
	__atomic_add_fetch(&reuse_zero_ticks, ticks, __ATOMIC_RELAXED);
}

/* Counts a reused run given again without zeroing, apart from the zeroed ones. */
void
bcm2711_buffer_kept(
	uint64_t bytes)
{
	/* One more kept run and its bytes. */
	__atomic_add_fetch(&kept_count, 1U, __ATOMIC_RELAXED);
	__atomic_add_fetch(&kept_bytes, bytes, __ATOMIC_RELAXED);
}

/*
 * Reports and clears the diagnostic sums of buffer creations and reuses
 * (stats[0..8]: made, bytes, allocator ticks, zeroing ticks, reused, reused
 * bytes, their zeroing ticks, kept unzeroed, their bytes).
 */
void
bcm2711_buffer_create_stats(
	uint64_t stats[9])
{
	/* Each sum is read and cleared on its own; a creation between them lands in the next report. */
	stats[0] = __atomic_exchange_n(&create_count, 0U, __ATOMIC_RELAXED);
	stats[1] = __atomic_exchange_n(&create_bytes, 0U, __ATOMIC_RELAXED);
	stats[2] = __atomic_exchange_n(&create_pmem_ticks, 0U, __ATOMIC_RELAXED);
	stats[3] = __atomic_exchange_n(&create_zero_ticks, 0U, __ATOMIC_RELAXED);
	stats[4] = __atomic_exchange_n(&reuse_count, 0U, __ATOMIC_RELAXED);
	stats[5] = __atomic_exchange_n(&reuse_bytes, 0U, __ATOMIC_RELAXED);
	stats[6] = __atomic_exchange_n(&reuse_zero_ticks, 0U, __ATOMIC_RELAXED);
	stats[7] = __atomic_exchange_n(&kept_count, 0U, __ATOMIC_RELAXED);
	stats[8] = __atomic_exchange_n(&kept_bytes, 0U, __ATOMIC_RELAXED);
}

/*
 * Reports and clears the diagnostic count, bytes and ticks of final buffer releases.
 */
void
bcm2711_buffer_release_stats(
	uint64_t *count,
	uint64_t *bytes,
	uint64_t *ticks)
{
	/* Each sum is read and cleared on its own; a release between them lands in the next report. */
	*count = __atomic_exchange_n(&release_count, 0U, __ATOMIC_RELAXED);
	*bytes = __atomic_exchange_n(&release_bytes, 0U, __ATOMIC_RELAXED);
	*ticks = __atomic_exchange_n(&release_ticks, 0U, __ATOMIC_RELAXED);
}
