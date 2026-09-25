/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Register access and forcewake (see mmio.h).
 *
 * The forcewake request is a masked write of the KERNEL bit to the domain's
 * request register; the domain is awake once the same bit reads back set in
 * its acknowledge register.  The request and acknowledge registers are always
 * on and are reached without any bookkeeping.
 */

#include "mmio.h"
#include "trace.h"

#include <kern/device-io.h>

#include <uapi/errno.h>
#include <stddef.h>

/* The KERNEL bit every forcewake request and acknowledge register uses. */
#define I915_FORCEWAKE_KERNEL		0x00000001U

/* The render and GT domains' request and acknowledge registers. */
#define I915_FORCEWAKE_REQUEST_RENDER	0x0000a278U
#define I915_FORCEWAKE_ACK_RENDER	0x00000d84U
#define I915_FORCEWAKE_REQUEST_GT	0x0000a188U
#define I915_FORCEWAKE_ACK_GT		0x00130044U

/* FORCEWAKE_MEDIA_VDBOX_GEN11(n): request 0xa540 + 4n, acknowledge 0xd50 + 4n. */
#define I915_FORCEWAKE_REQUEST_VDBOX0	0x0000a540U
#define I915_FORCEWAKE_ACK_VDBOX0	0x00000d50U
#define I915_FORCEWAKE_REQUEST_VDBOX2	0x0000a548U
#define I915_FORCEWAKE_ACK_VDBOX2	0x00000d58U

/* FORCEWAKE_MEDIA_VEBOX_GEN11(n): request 0xa560 + 4n, acknowledge 0xd70 + 4n. */
#define I915_FORCEWAKE_REQUEST_VEBOX0	0x0000a560U
#define I915_FORCEWAKE_ACK_VEBOX0	0x00000d70U

/* The value a read returns when the register cannot be reached. */
#define I915_MMIO_UNREADABLE		0xffffffffU

/*
 * The Gen12 register-to-domain map.
 *
 * It is the reference's table for Alder Lake-P and Tiger Lake and never
 * changes, so every device shares it.
 */
static const struct i915_mmio_range i915_gen12_ranges[] = {
#include "intel/forcewake-ranges.inc"
};

static void i915_mmio_note(struct i915_mmio *mmio, uint16_t op, const char *what, uint64_t argument0, uint64_t argument1);
static uint32_t i915_window_read32(void *context, uint32_t offset);
static void i915_window_write32(void *context, uint32_t offset, uint32_t value);
static void i915_window_forcewake_request(void *context, int domain, int wake);
static int i915_window_forcewake_ack(void *context, int domain);
static uint32_t i915_forcewake_request_register(int domain);
static uint32_t i915_forcewake_ack_register(int domain);

/*
 * Prepares the register access of one device with every domain released.
 */
void
drv_i915_mmio_init(
	struct i915_mmio *mmio,
	const struct i915_mmio_ops *ops,
	void *context,
	const struct i915_mmio_range *ranges,
	unsigned range_count,
	struct i915_trace *trace)
{
	int domain;

	/* Binds the bus access and the domain map. */
	mmio->ops = ops;
	mmio->context = context;
	mmio->trace = trace;
	mmio->ranges = ranges;
	mmio->range_count = range_count;

	/* Starts with no domain held. */
	for (domain = 0; domain < I915_FORCEWAKE_DOMAIN_COUNT; domain++) {
		mmio->forcewake_count[domain] = 0;
		mmio->forcewake_awake[domain] = 0;
	}

	/* Starts with clean failure counters and no steering section. */
	mmio->forcewake_ack_timeouts = 0U;
	mmio->forcewake_underflow = 0;
	mmio->mcr_locked = 0;
	mmio->mcr_steer = 0U;
}

/*
 * Returns the operations that reach a mapped register window.
 *
 * The context those operations receive is a struct i915_mmio_window.
 */
const struct i915_mmio_ops *
drv_i915_mmio_window_ops(void)
{
	static const struct i915_mmio_ops ops = {
		i915_window_read32,
		i915_window_write32,
		i915_window_forcewake_request,
		i915_window_forcewake_ack
	};

	/* Succeeded: the operations reach the mapped BAR. */
	return &ops;
}

/*
 * Returns the Gen12 register-to-domain map and its length.
 */
const struct i915_mmio_range *
drv_i915_mmio_gen12_ranges(
	unsigned *count)
{
	/* Reports the table length alongside the table. */
	*count = sizeof(i915_gen12_ranges) / sizeof(i915_gen12_ranges[0]);

	/* Succeeded: the table is static and shared. */
	return i915_gen12_ranges;
}

/*
 * Reports which forcewake domain a register belongs to, or -1 for always on.
 */
int
drv_i915_mmio_domain_of(
	const struct i915_mmio *mmio,
	uint32_t offset)
{
	unsigned index;

	/* Looks the register up in the domain map. */
	for (index = 0U; index < mmio->range_count; index++) {
		if (offset < mmio->ranges[index].start)
			continue;
		if (offset > mmio->ranges[index].end)
			continue;

		/* Succeeded: the register sits in this range's domain. */
		return mmio->ranges[index].domain;
	}

	/* A register in no range is always on. */
	return -1;
}

/*
 * Takes one hold on a forcewake domain, waking it on the first hold.
 *
 * Returns 0, EINVAL for an unknown domain, or ETIMEDOUT when the domain never
 * acknowledged; a failed first hold is not counted.
 */
int
drv_i915_forcewake_get(
	struct i915_mmio *mmio,
	int domain)
{
	unsigned poll;
	int acknowledged;

	/* Refuses a domain the driver does not know. */
	if (domain < 0 || domain >= I915_FORCEWAKE_DOMAIN_COUNT)
		return EINVAL;

	/*
	 * The count is the number of holders; a nested hold finds the domain
	 * already awake.
	 */
	i915_mmio_note(mmio, I915_TRACE_ACQUIRE, "forcewake", (uint64_t)domain, (uint64_t)(mmio->forcewake_count[domain] + 1));
	mmio->forcewake_count[domain]++;
	if (mmio->forcewake_count[domain] > 1)
		return 0;

	/* Asks the domain to wake. */
	mmio->ops->forcewake_request(mmio->context, domain, 1);

	/* Waits for the domain to acknowledge. */
	for (poll = 0U; poll < I915_FORCEWAKE_ACK_POLLS; poll++) {
		acknowledged = mmio->ops->forcewake_ack(mmio->context, domain);
		if (acknowledged != 0) {
			mmio->forcewake_awake[domain] = 1;
			return 0;
		}
	}

	/* The acknowledge never came: the hold is taken back so the count stays true. */
	mmio->forcewake_ack_timeouts++;
	mmio->forcewake_count[domain]--;
	i915_mmio_note(mmio, I915_TRACE_FAIL, "forcewake_ack", (uint64_t)domain, (uint64_t)ETIMEDOUT);

	return ETIMEDOUT;
}

/*
 * Gives back one hold on a forcewake domain, letting it sleep after the last.
 *
 * Returns 0, or EINVAL for an unknown domain or a release nobody held.
 */
int
drv_i915_forcewake_put(
	struct i915_mmio *mmio,
	int domain)
{
	/* Refuses a domain the driver does not know. */
	if (domain < 0 || domain >= I915_FORCEWAKE_DOMAIN_COUNT)
		return EINVAL;

	/* A release without a hold is a caller bug: recorded and refused. */
	if (mmio->forcewake_count[domain] == 0) {
		mmio->forcewake_underflow = 1;
		i915_mmio_note(mmio, I915_TRACE_FAIL, "forcewake_underflow", (uint64_t)domain, 0U);
		return EINVAL;
	}

	/* Drops the hold; the last one lets the domain sleep. */
	i915_mmio_note(mmio, I915_TRACE_RELEASE, "forcewake", (uint64_t)domain, (uint64_t)(mmio->forcewake_count[domain] - 1));
	mmio->forcewake_count[domain]--;
	if (mmio->forcewake_count[domain] == 0) {
		mmio->ops->forcewake_request(mmio->context, domain, 0);
		mmio->forcewake_awake[domain] = 0;
	}

	/* Succeeded: the hold is given back. */
	return 0;
}

/*
 * Reports nonzero while a forcewake domain has at least one holder.
 */
int
drv_i915_forcewake_held(
	const struct i915_mmio *mmio,
	int domain)
{
	/* An unknown domain is never held. */
	if (domain < 0 || domain >= I915_FORCEWAKE_DOMAIN_COUNT)
		return 0;

	/* A domain with holders is held. */
	if (mmio->forcewake_count[domain] > 0)
		return 1;

	/* Nobody holds the domain. */
	return 0;
}

/*
 * Reads a register whose domain the caller holds.
 *
 * A register whose domain is asleep is not read: the failure is recorded
 * and all-ones, what the hardware would return, is reported.
 */
uint32_t
drv_i915_read32(
	struct i915_mmio *mmio,
	uint32_t offset)
{
	uint32_t value;
	int domain;
	int held;

	/* Refuses a register whose domain nobody holds. */
	domain = drv_i915_mmio_domain_of(mmio, offset);
	if (domain >= 0) {
		held = drv_i915_forcewake_held(mmio, domain);
		if (held == 0) {
			i915_mmio_note(mmio, I915_TRACE_FAIL, "mmio_read_no_fw", offset, (uint64_t)domain);
			return I915_MMIO_UNREADABLE;
		}
	}

	/* Reads the register. */
	value = mmio->ops->read32(mmio->context, offset);

	/* Succeeded: reports the register value. */
	return value;
}

/*
 * Writes a register whose domain the caller holds.
 *
 * A write to a register whose domain is asleep is dropped and recorded.
 */
void
drv_i915_write32(
	struct i915_mmio *mmio,
	uint32_t offset,
	uint32_t value)
{
	int domain;
	int held;

	/* Refuses a register whose domain nobody holds. */
	domain = drv_i915_mmio_domain_of(mmio, offset);
	if (domain >= 0) {
		held = drv_i915_forcewake_held(mmio, domain);
		if (held == 0) {
			i915_mmio_note(mmio, I915_TRACE_FAIL, "mmio_write_no_fw", offset, (uint64_t)domain);
			return;
		}
	}

	/* Writes the register. */
	mmio->ops->write32(mmio->context, offset, value);
}

/*
 * Reads a register, holding its domain only for the read.
 */
uint32_t
drv_i915_read32_auto(
	struct i915_mmio *mmio,
	uint32_t offset)
{
	uint32_t value;
	int domain;

	/* Wakes the register's domain for the read. */
	domain = drv_i915_mmio_domain_of(mmio, offset);
	if (domain >= 0)
		(void)drv_i915_forcewake_get(mmio, domain);

	/* Reads the register. */
	value = mmio->ops->read32(mmio->context, offset);

	/* Lets the domain sleep again. */
	if (domain >= 0)
		(void)drv_i915_forcewake_put(mmio, domain);

	/* Succeeded: reports the register value. */
	return value;
}

/*
 * Writes a register, holding its domain only for the write.
 */
void
drv_i915_write32_auto(
	struct i915_mmio *mmio,
	uint32_t offset,
	uint32_t value)
{
	int domain;

	/* Wakes the register's domain for the write. */
	domain = drv_i915_mmio_domain_of(mmio, offset);
	if (domain >= 0)
		(void)drv_i915_forcewake_get(mmio, domain);

	/* Writes the register. */
	mmio->ops->write32(mmio->context, offset, value);

	/* Lets the domain sleep again. */
	if (domain >= 0)
		(void)drv_i915_forcewake_put(mmio, domain);
}

/*
 * Reads a register without any forcewake bookkeeping.
 *
 * Only for the init and reset paths, which hold every domain they touch.
 */
uint32_t
drv_i915_raw_read32(
	struct i915_mmio *mmio,
	uint32_t offset)
{
	uint32_t value;

	/* Reads the register as it is. */
	value = mmio->ops->read32(mmio->context, offset);

	/* Succeeded: reports the register value. */
	return value;
}

/*
 * Writes a register without any forcewake bookkeeping.
 *
 * Only for the init and reset paths, which hold every domain they touch.
 */
void
drv_i915_raw_write32(
	struct i915_mmio *mmio,
	uint32_t offset,
	uint32_t value)
{
	/* Writes the register as it is. */
	mmio->ops->write32(mmio->context, offset, value);
}

/*
 * Reads a register only to push an earlier write out to the device.
 */
void
drv_i915_posting_read32(
	struct i915_mmio *mmio,
	uint32_t offset)
{
	/* The value is not wanted; the read forces the preceding write to complete. */
	(void)mmio->ops->read32(mmio->context, offset);
	i915_mmio_note(mmio, I915_TRACE_NOTE, "posting_read", offset, 0U);
}

/*
 * Changes the masked bits of a register by reading, merging and writing back.
 *
 * For registers without the Intel write-enable half; the caller holds the
 * register's domain.
 */
void
drv_i915_rmw32(
	struct i915_mmio *mmio,
	uint32_t offset,
	uint32_t mask,
	uint32_t value)
{
	uint32_t old_value;
	uint32_t new_value;
	int domain;
	int held;

	/* Refuses a register whose domain nobody holds. */
	domain = drv_i915_mmio_domain_of(mmio, offset);
	if (domain >= 0) {
		held = drv_i915_forcewake_held(mmio, domain);
		if (held == 0) {
			i915_mmio_note(mmio, I915_TRACE_FAIL, "mmio_rmw_no_fw", offset, (uint64_t)domain);
			return;
		}
	}

	/* Keeps every bit outside the mask and replaces the bits inside it. */
	old_value = mmio->ops->read32(mmio->context, offset);
	new_value = (old_value & ~mask) | (value & mask);
	mmio->ops->write32(mmio->context, offset, new_value);
}

/*
 * Writes an Intel masked word: the upper half enables the lower half's bits.
 *
 * One write and no read; the hardware applies the mask.  The caller holds
 * the register's domain.
 */
void
drv_i915_write32_masked(
	struct i915_mmio *mmio,
	uint32_t offset,
	uint32_t masked_word)
{
	int domain;
	int held;

	/* Refuses a register whose domain nobody holds. */
	domain = drv_i915_mmio_domain_of(mmio, offset);
	if (domain >= 0) {
		held = drv_i915_forcewake_held(mmio, domain);
		if (held == 0) {
			i915_mmio_note(mmio, I915_TRACE_FAIL, "mmio_maskwrite_no_fw", offset, (uint64_t)domain);
			return;
		}
	}

	/* Writes the word; the mask travels in its upper half. */
	mmio->ops->write32(mmio->context, offset, masked_word);
}

/*
 * Opens the section in which replicated registers are steered to one instance.
 *
 * Returns 0, or EBUSY when a section is already open.
 */
int
drv_i915_mcr_lock(
	struct i915_mmio *mmio,
	uint32_t steer)
{
	/* A second steer inside an open section would redirect the first caller. */
	if (mmio->mcr_locked != 0) {
		i915_mmio_note(mmio, I915_TRACE_FAIL, "mcr_reentry", steer, mmio->mcr_steer);
		return EBUSY;
	}

	/* Records the section and its target. */
	mmio->mcr_locked = 1;
	mmio->mcr_steer = steer;
	i915_mmio_note(mmio, I915_TRACE_ACQUIRE, "mcr_steer", steer, 0U);

	/* Succeeded: the caller owns the steering until it unlocks. */
	return 0;
}

/*
 * Closes the steering section, if one is open.
 */
void
drv_i915_mcr_unlock(
	struct i915_mmio *mmio)
{
	/* Nothing is open. */
	if (mmio->mcr_locked == 0)
		return;

	/* Ends the section. */
	mmio->mcr_locked = 0;
	i915_mmio_note(mmio, I915_TRACE_RELEASE, "mcr_steer", mmio->mcr_steer, 0U);
}

/*
 * Reports nonzero while a steering section is open.
 */
int
drv_i915_mcr_locked(
	const struct i915_mmio *mmio)
{
	/* Reports the section state. */
	return mmio->mcr_locked;
}

/* Records a register access event when the device has a trace. */
static void
i915_mmio_note(
	struct i915_mmio *mmio,
	uint16_t op,
	const char *what,
	uint64_t argument0,
	uint64_t argument1)
{
	/* A register block without a trace records nothing. */
	if (mmio->trace == NULL)
		return;

	drv_i915_trace_record(mmio->trace, 0U, op, what, argument0, argument1);
}

/* Reads one register of the mapped window; outside the window reads all-ones. */
static uint32_t
i915_window_read32(
	void *context,
	uint32_t offset)
{
	struct i915_mmio_window *window;
	uint32_t value;

	/* An offset past the window would read unrelated memory. */
	window = context;
	if ((unsigned long)offset > window->size - 4UL)
		return I915_MMIO_UNREADABLE;

	/* Reads the register through the uncached mapping. */
	value = kern_mmio_read32(window->base + offset);

	/* Succeeded: reports the register value. */
	return value;
}

/* Writes one register of the mapped window; a write outside it is dropped. */
static void
i915_window_write32(
	void *context,
	uint32_t offset,
	uint32_t value)
{
	struct i915_mmio_window *window;

	/* An offset past the window would write unrelated memory. */
	window = context;
	if ((unsigned long)offset > window->size - 4UL)
		return;

	/* Writes the register through the uncached mapping. */
	kern_mmio_write32(window->base + offset, value);
}

/* Asks a domain to wake or lets it sleep through its request register. */
static void
i915_window_forcewake_request(
	void *context,
	int domain,
	int wake)
{
	struct i915_mmio_window *window;
	uint32_t request;
	uint32_t word;

	/* A masked write that changes only the KERNEL bit. */
	window = context;
	word = I915_FORCEWAKE_KERNEL << 16;
	if (wake != 0)
		word |= I915_FORCEWAKE_KERNEL;

	/* Writes the request. */
	request = i915_forcewake_request_register(domain);
	kern_mmio_write32(window->base + request, word);
}

/* Reports nonzero once a domain's acknowledge register shows the KERNEL bit. */
static int
i915_window_forcewake_ack(
	void *context,
	int domain)
{
	struct i915_mmio_window *window;
	uint32_t acknowledge;
	uint32_t value;

	/* Reads the domain's acknowledge register. */
	window = context;
	acknowledge = i915_forcewake_ack_register(domain);
	value = kern_mmio_read32(window->base + acknowledge);

	/* The domain is awake once the KERNEL bit is set. */
	if ((value & I915_FORCEWAKE_KERNEL) != 0U)
		return 1;

	/* Not acknowledged yet. */
	return 0;
}

/* Names the request register of a forcewake domain. */
static uint32_t
i915_forcewake_request_register(
	int domain)
{
	/* Maps each domain to its request register; the render domain is the default. */
	switch (domain) {
	case I915_FORCEWAKE_GT:
		return I915_FORCEWAKE_REQUEST_GT;
	case I915_FORCEWAKE_MEDIA_VDBOX0:
		return I915_FORCEWAKE_REQUEST_VDBOX0;
	case I915_FORCEWAKE_MEDIA_VDBOX2:
		return I915_FORCEWAKE_REQUEST_VDBOX2;
	case I915_FORCEWAKE_MEDIA_VEBOX0:
		return I915_FORCEWAKE_REQUEST_VEBOX0;
	default:
		return I915_FORCEWAKE_REQUEST_RENDER;
	}
}

/* Names the acknowledge register of a forcewake domain. */
static uint32_t
i915_forcewake_ack_register(
	int domain)
{
	/* Maps each domain to its acknowledge register; the render domain is the default. */
	switch (domain) {
	case I915_FORCEWAKE_GT:
		return I915_FORCEWAKE_ACK_GT;
	case I915_FORCEWAKE_MEDIA_VDBOX0:
		return I915_FORCEWAKE_ACK_VDBOX0;
	case I915_FORCEWAKE_MEDIA_VDBOX2:
		return I915_FORCEWAKE_ACK_VDBOX2;
	case I915_FORCEWAKE_MEDIA_VEBOX0:
		return I915_FORCEWAKE_ACK_VEBOX0;
	default:
		return I915_FORCEWAKE_ACK_RENDER;
	}
}
