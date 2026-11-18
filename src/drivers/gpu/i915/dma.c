/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * DMA mapping (see dma.h).
 *
 * The mapping bookkeeping, and the operations that bind it to the kernel's
 * drv_dma_* device.  Those operations check the requested mask against the
 * width the device can really address, never against a fixed host
 * physical-address width.  The mapping operations are not bound yet: they
 * stay NULL, so an attempt records an unimplemented operation instead of
 * inventing a DMA address.
 */

#include "i915.h"
#include "dma.h"
#include "trace.h"

#include <drivers/generic/dma.h>

#include <uapi/errno.h>
#include <stddef.h>

static void i915_dma_note(struct i915_dma *dma, uint16_t op, const char *what, uint64_t argument0, uint64_t argument1);
static struct i915_dma_mapping *i915_dma_claim_mapping(struct i915_dma *dma);
static int i915_dma_map_entries(struct i915_dma *dma, const struct i915_sg_entry *entries, unsigned orig_nents, enum i915_dma_direction direction, struct i915_dma_mapping **mapping, unsigned *nents);
static int i915_device_dma_set_info(void *context, unsigned mask_bits, uint64_t max_segment);

/*
 * Binds the DMA side of one device to its operations with no mapping held.
 *
 * It does not touch the hardware.
 */
void
drv_i915_dma_init(
	struct i915_dma *dma,
	const struct i915_dma_ops *ops,
	void *context,
	struct i915_trace *trace)
{
	unsigned index;

	/* Binds the address production and the trace. */
	dma->ops = ops;
	dma->context = context;
	dma->trace = trace;

	/* Starts with no mask accepted yet. */
	dma->mask_bits = 0U;
	dma->max_segment = 0U;
	dma->info_set = 0;

	/* Resource numbers start at 1, so a zero number never names a live mapping. */
	dma->next_resource_id = 1U;

	/* Starts with every mapping slot free. */
	for (index = 0U; index < I915_DMA_MAX_MAPPINGS; index++) {
		dma->mappings[index].in_use = 0;
		dma->mappings[index].pin_count = 0;
		dma->mappings[index].resource_id = 0U;
		dma->mappings[index].orig_nents = 0U;
		dma->mappings[index].nents = 0U;
	}
}

/*
 * Returns the operations that reach the kernel's DMA device.
 *
 * The context those operations receive is a struct drv_dma_device.  The
 * width, largest segment and coherence the table states are zero: the
 * device reports the real values at run time.
 */
const struct i915_dma_ops *
drv_i915_dma_device_ops(void)
{
	static const struct i915_dma_ops ops = {
		"zedbsd-dma",
		0U,
		0U,
		0,
		i915_device_dma_set_info,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL
	};

	/*
	 * Succeeded: only set_info is bound.  The mapping operations wait for
	 * the GGTT and PPGTT to use them.
	 */
	return &ops;
}

/*
 * Sets the device's DMA mask and largest segment.
 *
 * This is set_dma_info.  Returns 0, EINVAL for a mask or segment the call
 * cannot mean, EOPNOTSUPP when the operations leave it unimplemented, or the
 * refusal the operations report.
 */
int
drv_i915_dma_set_info(
	struct i915_dma *dma,
	unsigned mask_bits,
	uint64_t max_segment)
{
	int error;

	/* Records the request. */
	i915_dma_note(dma, I915_TRACE_ENTRY, "dma_set_info", mask_bits, max_segment);

	/*
	 * A DMA mask is a capability of the device, not the guest's
	 * physical-address width; a mask outside 32..64 bits or an empty
	 * segment is refused.
	 */
	if (mask_bits < 32U ||
	    mask_bits > 64U ||
	    max_segment == 0U) {
		i915_dma_note(dma, I915_TRACE_FAIL, "dma_set_info", (uint64_t)EINVAL, 0U);
		return EINVAL;
	}

	/* An unimplemented operation is recorded as such, not as a refusal by the hardware. */
	if (dma->ops->set_info == NULL) {
		i915_dma_note(dma, I915_TRACE_UNIMPLEMENTED, "dma_set_info", 0U, 0U);
		return EOPNOTSUPP;
	}

	/* Asks the device to accept the mask. */
	error = dma->ops->set_info(dma->context, mask_bits, max_segment);
	if (error != 0) {
		i915_dma_note(dma, I915_TRACE_FAIL, "dma_set_info", (uint64_t)error, 0U);
		return error;
	}

	/* Remembers what the device accepted. */
	dma->mask_bits = mask_bits;
	dma->max_segment = max_segment;
	dma->info_set = 1;
	i915_dma_note(dma, I915_TRACE_EXIT, "dma_set_info", 0U, 0U);

	/* Succeeded: the device takes addresses of the requested width. */
	return 0;
}

/*
 * Reports the device DMA address width the operations state.
 */
unsigned
drv_i915_dma_address_bits(
	const struct i915_dma *dma)
{
	/*
	 * XXX: the kernel device operations state 0 here and leave the real
	 * width to drv_dma_device_address_bits(); a caller on the real device
	 * receives 0.
	 */
	return dma->ops->address_bits;
}

/*
 * Reports the largest DMA segment the operations state.
 */
uint64_t
drv_i915_dma_max_segment(
	const struct i915_dma *dma)
{
	/* XXX: 0 on the real device, as for drv_i915_dma_address_bits(). */
	return dma->ops->max_segment;
}

/*
 * Reports nonzero when the operations state that mappings are cache coherent.
 */
int
drv_i915_dma_is_coherent(
	const struct i915_dma *dma)
{
	/* XXX: 0 on the real device, as for drv_i915_dma_address_bits(). */
	return dma->ops->coherent;
}

/*
 * Maps a scatter list for the device.
 *
 * This is dma_map_sg: returns the mapped segment count, at least 1, on
 * success and 0 on failure.  On success *mapping receives the owned mapping,
 * and on failure it is NULL.
 */
int
drv_i915_dma_map_sg(
	struct i915_dma *dma,
	const struct i915_sg_entry *entries,
	unsigned orig_nents,
	enum i915_dma_direction direction,
	struct i915_dma_mapping **mapping)
{
	unsigned nents;
	int error;

	/* Maps the entries into a mapping slot. */
	error = i915_dma_map_entries(dma, entries, orig_nents, direction, mapping, &nents);

	/* dma_map_sg reports every failure as zero segments. */
	if (error != 0)
		return 0;

	/* Succeeded: reports how many segments the hardware walk covers. */
	return (int)nents;
}

/*
 * Maps a scatter list for the device.
 *
 * This is dma_map_sgtable: returns 0 on success or a positive errno.  On
 * success *mapping receives the owned mapping, and on failure it is NULL.
 */
int
drv_i915_dma_map_sgtable(
	struct i915_dma *dma,
	const struct i915_sg_entry *entries,
	unsigned orig_nents,
	enum i915_dma_direction direction,
	struct i915_dma_mapping **mapping)
{
	unsigned nents;
	int error;

	/* Maps the entries into a mapping slot. */
	error = i915_dma_map_entries(dma, entries, orig_nents, direction, mapping, &nents);

	/* Reports why the list could not be mapped. */
	if (error != 0)
		return error;

	/* Succeeded: the caller owns the mapping. */
	return 0;
}

/*
 * Gives back a scatter mapping.
 *
 * Returns 0, EINVAL for a mapping that is not held, or EBUSY -- with nothing
 * given back -- while a GPU page table still holds a pin on it.
 */
int
drv_i915_dma_unmap_sg(
	struct i915_dma *dma,
	struct i915_dma_mapping *mapping)
{
	/* Refuses a mapping that does not exist. */
	if (mapping == NULL)
		return EINVAL;

	/* Refuses a slot that holds no mapping. */
	if (mapping->in_use == 0)
		return EINVAL;

	/* A mapping still bound into a GPU page table is never given back. */
	if (mapping->pin_count != 0) {
		i915_dma_note(dma, I915_TRACE_FAIL, "dma_unmap_sg", (uint64_t)EBUSY, mapping->resource_id);
		return EBUSY;
	}

	/* Gives the segments back to the device side when the operations can. */
	if (dma->ops->unmap_sg != NULL)
		dma->ops->unmap_sg(dma->context, mapping->segments, mapping->nents, mapping->orig_nents, mapping->direction);

	/* Records the unmap and the release of the mapping. */
	i915_dma_note(dma, I915_TRACE_UNMAP, "dma_unmap_sg", mapping->orig_nents, mapping->resource_id);
	i915_dma_note(dma, I915_TRACE_RELEASE, "dma_mapping", mapping->resource_id, 0U);

	/* Frees the slot for the next mapping. */
	mapping->in_use = 0;

	/* Succeeded: the device no longer reaches the list. */
	return 0;
}

/*
 * Maps one physical run for the device.
 *
 * The result is tested with drv_i915_dma_mapping_failed().
 */
i915_dma_addr_t
drv_i915_dma_map_page(
	struct i915_dma *dma,
	i915_cpu_phys_t phys,
	uint32_t size,
	enum i915_dma_direction direction)
{
	uint64_t address;

	/* Records the request. */
	i915_dma_note(dma, I915_TRACE_ENTRY, "dma_map_page", phys.value, size);

	/* An unimplemented operation is recorded as such and fails the mapping. */
	if (dma->ops->map_page == NULL) {
		i915_dma_note(dma, I915_TRACE_UNIMPLEMENTED, "dma_map_page", 0U, 0U);
		return drv_i915_dma_addr(I915_DMA_MAPPING_ERROR);
	}

	/* Asks the device side for the run's device address. */
	address = dma->ops->map_page(dma->context, phys.value, size, direction);
	if (address == I915_DMA_MAPPING_ERROR) {
		i915_dma_note(dma, I915_TRACE_FAIL, "dma_map_page", 0U, 0U);
		return drv_i915_dma_addr(I915_DMA_MAPPING_ERROR);
	}

	/* Records which CPU address became which device address. */
	i915_dma_note(dma, I915_TRACE_MAP, "dma_map_page", phys.value, address);

	/* Succeeded: the device reaches the run at this address. */
	return drv_i915_dma_addr(address);
}

/*
 * Gives back one page mapping.
 */
void
drv_i915_dma_unmap_page(
	struct i915_dma *dma,
	i915_dma_addr_t address,
	uint32_t size,
	enum i915_dma_direction direction)
{
	/* Gives the run back to the device side when the operations can. */
	if (dma->ops->unmap_page != NULL)
		dma->ops->unmap_page(dma->context, address.value, size, direction);

	/* Records the unmap. */
	i915_dma_note(dma, I915_TRACE_UNMAP, "dma_unmap_page", address.value, 0U);
}

/*
 * Pins a scatter mapping into a GPU page table.
 *
 * Returns the device address of the first segment -- the value that belongs
 * in a page table entry -- or I915_DMA_MAPPING_ERROR for a mapping that is
 * not held.  A CPU physical address can never leave through here.
 */
i915_dma_addr_t
drv_i915_dma_pin(
	struct i915_dma *dma,
	struct i915_dma_mapping *mapping)
{
	/* Refuses a mapping that does not exist. */
	if (mapping == NULL)
		return drv_i915_dma_addr(I915_DMA_MAPPING_ERROR);

	/* Refuses a slot that holds no mapping. */
	if (mapping->in_use == 0)
		return drv_i915_dma_addr(I915_DMA_MAPPING_ERROR);

	/*
	 * The pin keeps the mapping from being given back while a page table
	 * entry points into it.
	 */
	mapping->pin_count++;
	i915_dma_note(dma, I915_TRACE_ACQUIRE, "dma_pin", mapping->resource_id, mapping->segments[0].address.value);

	/* Succeeded: only a mapped device address leaves here. */
	return mapping->segments[0].address;
}

/*
 * Gives back one pin on a scatter mapping.
 *
 * A mapping that is not held or not pinned is left alone.
 */
void
drv_i915_dma_unpin(
	struct i915_dma *dma,
	struct i915_dma_mapping *mapping)
{
	/* Ignores a mapping that does not exist. */
	if (mapping == NULL)
		return;

	/* Ignores a slot that holds no mapping. */
	if (mapping->in_use == 0)
		return;

	/* Ignores a mapping nobody pinned. */
	if (mapping->pin_count == 0)
		return;

	/* Drops the pin; the last one lets the mapping be given back. */
	mapping->pin_count--;
	i915_dma_note(dma, I915_TRACE_RELEASE, "dma_pin", mapping->resource_id, 0U);
}

/*
 * Hands a run the CPU wrote over to the device.
 */
void
drv_i915_dma_sync_for_device(
	struct i915_dma *dma,
	i915_dma_addr_t address,
	uint32_t size,
	enum i915_dma_direction direction)
{
	/*
	 * Flushes the run toward the device when the operations can.
	 *
	 * XXX: without the operation nothing is flushed, yet the sync is still
	 * recorded as done.
	 */
	if (dma->ops->sync_for_device != NULL)
		dma->ops->sync_for_device(dma->context, address.value, size, direction);

	/* Records the hand-over. */
	i915_dma_note(dma, I915_TRACE_SYNC_DEVICE, "dma_sync_for_device", address.value, size);
}

/*
 * Hands a run the device wrote back to the CPU.
 */
void
drv_i915_dma_sync_for_cpu(
	struct i915_dma *dma,
	i915_dma_addr_t address,
	uint32_t size,
	enum i915_dma_direction direction)
{
	/*
	 * Makes the device's writes visible to the CPU when the operations can.
	 *
	 * XXX: without the operation nothing is invalidated, yet the sync is
	 * still recorded as done.
	 */
	if (dma->ops->sync_for_cpu != NULL)
		dma->ops->sync_for_cpu(dma->context, address.value, size, direction);

	/* Records the hand-back. */
	i915_dma_note(dma, I915_TRACE_SYNC_CPU, "dma_sync_for_cpu", address.value, size);
}

/*
 * Reports how many mappings are held and not yet given back.
 *
 * A nonzero count after every user has finished is a leak.
 */
unsigned
drv_i915_dma_live_mappings(
	const struct i915_dma *dma)
{
	unsigned index;
	unsigned count;

	/* Counts the slots that hold a mapping. */
	count = 0U;
	for (index = 0U; index < I915_DMA_MAX_MAPPINGS; index++) {
		if (dma->mappings[index].in_use != 0)
			count++;
	}

	/* Succeeded: reports the number of live mappings. */
	return count;
}

/* Records one DMA event when the device has a trace. */
static void
i915_dma_note(
	struct i915_dma *dma,
	uint16_t op,
	const char *what,
	uint64_t argument0,
	uint64_t argument1)
{
	/* A device without a trace records nothing. */
	if (dma->trace == NULL)
		return;

	/* Appends the record to the device trace. */
	drv_i915_trace_record(dma->trace, 0U, op, what, argument0, argument1);
}

/* Claims a free mapping slot and gives it a new resource number, or returns NULL. */
static struct i915_dma_mapping *
i915_dma_claim_mapping(
	struct i915_dma *dma)
{
	struct i915_dma_mapping *mapping;
	unsigned index;

	/* Takes the first slot that holds no mapping. */
	for (index = 0U; index < I915_DMA_MAX_MAPPINGS; index++) {
		mapping = &dma->mappings[index];
		if (mapping->in_use != 0)
			continue;

		/*
		 * The slot is held from here and carries a number no earlier
		 * mapping had; it has no pin and no segments yet.
		 */
		mapping->in_use = 1;
		mapping->pin_count = 0;
		mapping->resource_id = dma->next_resource_id;
		dma->next_resource_id++;
		mapping->orig_nents = 0U;
		mapping->nents = 0U;

		/* Succeeded: the caller fills the claimed slot. */
		return mapping;
	}

	/* Every slot holds a mapping. */
	return NULL;
}

/*
 * Maps a scatter list into a mapping slot; returns 0 or a positive errno and
 * the produced segment count.
 */
static int
i915_dma_map_entries(
	struct i915_dma *dma,
	const struct i915_sg_entry *entries,
	unsigned orig_nents,
	enum i915_dma_direction direction,
	struct i915_dma_mapping **mapping,
	unsigned *nents)
{
	struct i915_dma_mapping *claimed;
	unsigned segment;
	uint64_t phys;
	int produced;

	/* Nothing is handed out unless the mapping completes. */
	*mapping = NULL;
	*nents = 0U;

	/* Records the request. */
	i915_dma_note(dma, I915_TRACE_ENTRY, "dma_map_sg", orig_nents, (uint64_t)direction);

	/* Refuses a missing list. */
	if (entries == NULL)
		return EINVAL;

	/* Refuses an empty list. */
	if (orig_nents == 0U)
		return EINVAL;

	/* Refuses a list longer than one mapping can hold. */
	if (orig_nents > I915_DMA_MAX_SEGMENTS)
		return EINVAL;

	/* An unimplemented operation is recorded as such, not as a refusal by the hardware. */
	if (dma->ops->map_sg == NULL) {
		i915_dma_note(dma, I915_TRACE_UNIMPLEMENTED, "dma_map_sg", 0U, 0U);
		return EOPNOTSUPP;
	}

	/* Claims the slot the mapping is kept in. */
	claimed = i915_dma_claim_mapping(dma);
	if (claimed == NULL)
		return ENOMEM;

	/*
	 * Asks the device side for the segments.  On failure the slot is
	 * freed again and the caller must not go on to bind or submit.
	 */
	produced = dma->ops->map_sg(dma->context, entries, orig_nents, claimed->segments, I915_DMA_MAX_SEGMENTS, direction);
	if (produced <= 0) {
		claimed->in_use = 0;
		i915_dma_note(dma, I915_TRACE_FAIL, "dma_map_sg", 0U, 0U);
		return ENOMEM;
	}

	/*
	 * Keeps the original entry count for the unmap and the coalesced
	 * segment count for the hardware walk.
	 */
	claimed->direction = direction;
	claimed->orig_nents = orig_nents;
	claimed->nents = (unsigned)produced;

	/*
	 * Records which CPU address became which device address.  A segment
	 * past the last entry is recorded against the first entry.
	 */
	for (segment = 0U; segment < claimed->nents; segment++) {
		if (segment < orig_nents) {
			phys = entries[segment].phys.value;
		} else {
			phys = entries[0].phys.value;
		}
		i915_dma_note(dma, I915_TRACE_MAP, "dma_map_sg", phys, claimed->segments[segment].address.value);
	}

	/* Hands the completed mapping to the caller. */
	*mapping = claimed;
	*nents = claimed->nents;
	i915_dma_note(dma, I915_TRACE_EXIT, "dma_map_sg", (uint64_t)claimed->nents, claimed->resource_id);

	/* Succeeded: the caller owns the mapping. */
	return 0;
}

/* Accepts a DMA mask only when the kernel's DMA device can address that many bits. */
static int
i915_device_dma_set_info(
	void *context,
	unsigned mask_bits,
	uint64_t max_segment)
{
	struct drv_dma_device *device;
	unsigned device_bits;

	UNUSED_PARAMETER(max_segment);

	/* Asks the device how wide an address it can put on the bus. */
	device = context;
	device_bits = drv_dma_device_address_bits(device);

	/* Refuses an empty mask. */
	if (mask_bits == 0U)
		return EINVAL;

	/* Refuses a mask wider than the device can address. */
	if (mask_bits > device_bits)
		return EINVAL;

	/* Succeeded: the device can address the requested width. */
	return 0;
}
