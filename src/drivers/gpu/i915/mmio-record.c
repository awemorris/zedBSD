/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The record of the display register writes of a device's start (see
 * mmio-record.h).
 *
 * The writes come from every thread of the start and from the interrupt
 * handler, so the ring is guarded by a flag taken with the local interrupts
 * off.  It is not a ranked lock: a register write may be made under any
 * lock, and nothing else is taken while the flag is held.
 */

#include "mmio-record.h"
#include "mmio.h"

#include <kern/irq.h>
#include <kern/kcrt.h>
#include <kern/klog.h>

#include <stdbool.h>
#include <stddef.h>

/* The Gen12 register layout, the only one whose display writes are kept. */
#define I915_MMIO_RECORD_GRAPHICS_VER	12

/* How many register-value pairs one line of the whole record shows. */
#define I915_MMIO_RECORD_LINE_PAIRS	6U

/* What a display write is to the record. */
enum i915_mmio_record_kind {
	I915_MMIO_RECORD_IGNORED,
	I915_MMIO_RECORD_KEPT,
	I915_MMIO_RECORD_INTERRUPT
};

/*
 * The record of one device's start.
 *
 * owner is the register block of the first Gen12 device that wrote a
 * display register; the writes of any other block are ignored.  The ring
 * fills from entry 0 and is never overwritten: a write that finds it full
 * is counted as dropped.  run_first and run_length describe the writes to
 * consecutive registers that ended the ring, so a run longer than
 * I915_MMIO_RECORD_RUN is folded into one entry as it grows.
 */
struct i915_mmio_record {
	const struct i915_mmio *owner;
	struct i915_mmio_record_entry entry[I915_MMIO_RECORD_ENTRIES];
	unsigned used;
	unsigned run_first;
	unsigned run_length;
	uint32_t interrupt_writes;
	uint32_t dropped;
	uint32_t writes_seen;
	int closed_early;
};

/*
 * Nonzero while the record is open; see mmio-record.h.  It starts set, so the
 * first write of the first device is kept, and is cleared once when the
 * record is reported or closes itself.
 */
volatile int drv_i915_mmio_record_on = 1;

/*
 * The record itself.  Every field is changed only while i915_mmio_record_busy
 * is held; it is zero (no owner, empty) until the first display write.
 */
static struct i915_mmio_record i915_mmio_record;

/*
 * The guard of the record: 1 while one CPU changes or reads the record, with
 * its local interrupts off so the interrupt handler's write cannot spin on
 * the CPU that holds it.
 */
static volatile int i915_mmio_record_busy;

static enum i915_mmio_record_kind i915_mmio_record_classify(uint32_t offset);
static int i915_mmio_record_enter(void);
static void i915_mmio_record_leave(int enabled);
static void i915_mmio_record_append_locked(uint32_t offset, uint32_t value);
static void i915_mmio_record_fold_locked(void);
static void i915_mmio_record_close_locked(int early);
static uint32_t i915_mmio_record_mix(uint32_t offset, uint32_t value);

/*
 * Hands one register write to the record.
 *
 * Called by the register access for every write while the record is open,
 * after the write reached the hardware.  It is kept out of line, so the
 * register access itself stays a bus write and a test of the open flag.
 */
__attribute__((noinline)) void
drv_i915_mmio_record_write(
	const struct i915_mmio *mmio,
	uint32_t offset,
	uint32_t value)
{
	enum i915_mmio_record_kind kind;
	int enabled;

	/* Only a Gen12 device's display registers are recorded. */
	kind = I915_MMIO_RECORD_IGNORED;
	if (mmio->graphics_ver == I915_MMIO_RECORD_GRAPHICS_VER)
		kind = i915_mmio_record_classify(offset);

	/* Counts every write, and keeps a display write of the owning device. */
	enabled = i915_mmio_record_enter();

	if (drv_i915_mmio_record_on != 0) {
		i915_mmio_record.writes_seen++;
		if (kind != I915_MMIO_RECORD_IGNORED && i915_mmio_record.owner == NULL)
			i915_mmio_record.owner = mmio;

		/* Keeps a write of the owner's display. */
		if (i915_mmio_record.owner == mmio) {
			if (kind == I915_MMIO_RECORD_KEPT) {
				i915_mmio_record_append_locked(offset, value);
			} else if (kind == I915_MMIO_RECORD_INTERRUPT) {
				i915_mmio_record.interrupt_writes++;
			}
		}

		/* A device that never reports the record stops paying for it. */
		if (i915_mmio_record.writes_seen >= I915_MMIO_RECORD_WRITE_LIMIT)
			i915_mmio_record_close_locked(1);
	}

	i915_mmio_record_leave(enabled);
}

/*
 * Puts a name between the recorded writes.
 *
 * The name says which part of the display code makes the writes that follow;
 * it must outlive the record (a string constant).
 */
void
drv_i915_mmio_record_mark(
	const char *name)
{
	struct i915_mmio_record_entry *entry;
	int enabled;

	/* A closed record takes no more names. */
	if (drv_i915_mmio_record_on == 0)
		return;

	/* Appends the name as an entry of its own; a full ring drops it. */
	enabled = i915_mmio_record_enter();

	if (drv_i915_mmio_record_on != 0 && i915_mmio_record.used < I915_MMIO_RECORD_ENTRIES) {
		entry = &i915_mmio_record.entry[i915_mmio_record.used];
		entry->offset = 0U;
		entry->value = 0U;
		entry->count = 0U;
		entry->mark = name;
		i915_mmio_record.used++;

		/* A name ends a run of consecutive registers. */
		i915_mmio_record.run_length = 0U;
	}

	i915_mmio_record_leave(enabled);
}

/*
 * Closes the record and reports what it holds.
 *
 * Closing a record that is already closed only reports it again.
 */
void
drv_i915_mmio_record_close(
	struct i915_mmio_record_digest *digest)
{
	const struct i915_mmio_record_entry *entry;
	uint32_t order;
	uint32_t set;
	uint32_t writes;
	uint32_t runs;
	unsigned i;
	int enabled;

	/* Closes the record, so the entries no longer change. */
	enabled = i915_mmio_record_enter();

	if (drv_i915_mmio_record_on != 0)
		i915_mmio_record_close_locked(0);

	i915_mmio_record_leave(enabled);

	/*
	 * Digests every kept write: FNV-1a over the mixed register-value
	 * pairs in order, and their wrapping sum, which no order changes.
	 */
	order = 2166136261U;
	set = 0U;
	writes = 0U;
	runs = 0U;
	for (i = 0U; i < i915_mmio_record.used; i++) {
		entry = &i915_mmio_record.entry[i];
		if (entry->count == 0U)
			continue;

		/* Folds the entry into both sums. */
		order ^= i915_mmio_record_mix(entry->offset, entry->value);
		order *= 16777619U;
		set += i915_mmio_record_mix(entry->offset, entry->value);
		writes += entry->count;

		/* A run stands for several writes. */
		if (entry->count > 1U)
			runs++;
	}

	/* Reports the numbers. */
	digest->writes = writes;
	digest->entries = i915_mmio_record.used;
	digest->runs = runs;
	digest->interrupt_writes = i915_mmio_record.interrupt_writes;
	digest->dropped = i915_mmio_record.dropped;
	digest->order_digest = order;
	digest->set_digest = set;
	digest->closed_early = i915_mmio_record.closed_early;
}

/*
 * Copies the entries of a closed record, at most max of them.
 *
 * Returns how many were copied; an open record reports none, as its entries
 * may still change.
 */
unsigned
drv_i915_mmio_record_entries(
	struct i915_mmio_record_entry *out,
	unsigned max)
{
	unsigned count;
	unsigned i;

	/* The entries of an open record may still change. */
	if (drv_i915_mmio_record_on != 0)
		return 0U;

	/* Copies the oldest entries first. */
	count = i915_mmio_record.used;
	if (count > max)
		count = max;
	for (i = 0U; i < count; i++)
		out[i] = i915_mmio_record.entry[i];

	/* Succeeded: the number of entries copied. */
	return count;
}

/*
 * Closes the record and writes it to the kernel log.
 *
 * The digest line is always written; whole asks for every entry too, six
 * register-value pairs to a line and each name on a line of its own.
 */
void
drv_i915_mmio_record_log(
	int whole)
{
	struct i915_mmio_record_digest digest;
	const struct i915_mmio_record_entry *entry;
	char line[160];
	unsigned length;
	unsigned pairs;
	unsigned i;
	int written;

	/* Closes the record and digests it. */
	drv_i915_mmio_record_close(&digest);

	/* Reports the digest, which two starts are compared by. */
	kern_logf("i915: w: %u writes in %u entries (%u runs), %u interrupt-register writes counted only, %u dropped%s; order 0x%08x set 0x%08x\n",
	    digest.writes,
	    digest.entries,
	    digest.runs,
	    digest.interrupt_writes,
	    digest.dropped,
	    digest.closed_early ? ", closed before the first picture" : "",
	    digest.order_digest,
	    digest.set_digest);

	/* The digest alone was asked for. */
	if (!whole)
		return;

	/* Writes every entry, a few to a line. */
	length = 0U;
	pairs = 0U;
	for (i = 0U; i < digest.entries; i++) {
		entry = &i915_mmio_record.entry[i];

		/* A name, or a full line, ends the line under way. */
		if (pairs != 0U &&
		    (entry->count == 0U ||
		     pairs == I915_MMIO_RECORD_LINE_PAIRS)) {
			kern_logf("i915: w%s\n", line);
			length = 0U;
			pairs = 0U;
		}

		/* A name stands on its own line. */
		if (entry->count == 0U) {
			kern_logf("i915: w step %s\n", entry->mark);
			continue;
		}

		/* Adds the write, or the run with its length and the sum of its values. */
		if (entry->count == 1U) {
			written = kern_snprintf(line + length, sizeof(line) - length, " %05x=%08x", entry->offset, entry->value);
		} else {
			written = kern_snprintf(line + length, sizeof(line) - length, " %05x[%u]+%08x", entry->offset, entry->count, entry->value);
		}

		/* Moves past what was written. */
		if (written > 0)
			length += (unsigned)written;
		pairs++;
	}

	/* Writes the last, partly filled line. */
	if (pairs != 0U)
		kern_logf("i915: w%s\n", line);
}

/*
 * Empties the record and opens it again.
 *
 * Only for the host tests, which record several starts in one process.
 */
void
drv_i915_mmio_record_reset(void)
{
	int enabled;

	/* Forgets the owner and every entry, and opens the record. */
	enabled = i915_mmio_record_enter();

	i915_mmio_record.owner = NULL;
	i915_mmio_record.used = 0U;
	i915_mmio_record.run_first = 0U;
	i915_mmio_record.run_length = 0U;
	i915_mmio_record.interrupt_writes = 0U;
	i915_mmio_record.dropped = 0U;
	i915_mmio_record.writes_seen = 0U;
	i915_mmio_record.closed_early = 0;
	drv_i915_mmio_record_on = 1;

	i915_mmio_record_leave(enabled);
}

/*
 * Tells whether a register write is kept, counted only, or not a display write.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (GEN11_DISPLAY_INT_CTL, GEN8_DE_PIPE_ISR to GEN11_DE_HPD_IER, SDEISR to
 * SDEIER, HIP_INDEX_REG, GEN6_PCODE_MAILBOX to GEN6_PCODE_DATA1) and
 * drivers/gpu/drm/i915/display/intel_dmc_regs.h (DMC_MMIO_END_RANGE,
 * _TGL_PIPEB_MMIO_END); the display blocks are 0x40000 to 0x9ffff (pipes,
 * planes, ports, DMC), 0xc0000 to 0xcffff (south display) and 0x160000 to
 * 0x16ffff (PHYs and PLLs).
 */
static enum i915_mmio_record_kind
i915_mmio_record_classify(
	uint32_t offset)
{
	/* The display interrupt control and the north interrupt registers. */
	if (offset == 0x44200U)
		return I915_MMIO_RECORD_INTERRUPT;
	if (offset >= 0x44400U && offset < 0x44480U)
		return I915_MMIO_RECORD_INTERRUPT;

	/* The south interrupt registers. */
	if (offset >= 0xc4000U && offset < 0xc4010U)
		return I915_MMIO_RECORD_INTERRUPT;

	/* The north display, its DMC included. */
	if (offset >= 0x40000U && offset < 0xa0000U)
		return I915_MMIO_RECORD_KEPT;

	/* The south display. */
	if (offset >= 0xc0000U && offset < 0xd0000U)
		return I915_MMIO_RECORD_KEPT;

	/* The Type-C PHY window index. */
	if (offset >= 0x1010a0U && offset < 0x1010a8U)
		return I915_MMIO_RECORD_KEPT;

	/* The GT driver mailbox, which the display's clock and bandwidth steps use. */
	if (offset >= 0x138124U && offset < 0x138130U)
		return I915_MMIO_RECORD_KEPT;

	/* The PHYs, the PLLs and their clock routing. */
	if (offset >= 0x160000U && offset < 0x170000U)
		return I915_MMIO_RECORD_KEPT;

	/* Any other register is not the display's. */
	return I915_MMIO_RECORD_IGNORED;
}

/* Takes the record's guard with the local interrupts off; returns whether they were on. */
static int
i915_mmio_record_enter(void)
{
	bool was_enabled;
	int enabled;
	int taken;

	/* Keeps the interrupt handler of this CPU out while the guard is held. */
	was_enabled = kern_irq_disable();
	enabled = 0;
	if (was_enabled)
		enabled = 1;

	/* Takes the guard, waiting for another CPU's change to end. */
	for (;;) {
		taken = __atomic_exchange_n(&i915_mmio_record_busy, 1, __ATOMIC_ACQUIRE);
		if (taken == 0)
			break;

		/* Spins on a plain read until the holder lets go. */
		while (i915_mmio_record_busy != 0)
			;
	}

	/* Succeeded: the guard is held. */
	return enabled;
}

/* Releases the record's guard and gives the local interrupts back. */
static void
i915_mmio_record_leave(
	int enabled)
{
	/* Lets another CPU change the record. */
	__atomic_store_n(&i915_mmio_record_busy, 0, __ATOMIC_RELEASE);

	/* Turns the local interrupts back on if they were on. */
	if (enabled)
		kern_irq_enable();
}

/* Keeps one display write, extending the run it continues. */
static void
i915_mmio_record_append_locked(
	uint32_t offset,
	uint32_t value)
{
	struct i915_mmio_record_entry *tail;
	struct i915_mmio_record_entry *entry;

	/* A run already folded into one entry grows by the write. */
	if (i915_mmio_record.run_length >= I915_MMIO_RECORD_RUN) {
		tail = &i915_mmio_record.entry[i915_mmio_record.run_first];
		if (offset == tail->offset + 4U * tail->count) {
			tail->count++;
			tail->value += value;
			i915_mmio_record.run_length++;
			return;
		}
	}

	/* A full ring keeps nothing more. */
	if (i915_mmio_record.used >= I915_MMIO_RECORD_ENTRIES) {
		i915_mmio_record.dropped++;
		return;
	}

	/* Decides whether the write continues the consecutive registers before it. */
	if (i915_mmio_record.run_length != 0U && i915_mmio_record.run_length < I915_MMIO_RECORD_RUN) {
		tail = &i915_mmio_record.entry[i915_mmio_record.used - 1U];
		if (tail->count == 1U && offset == tail->offset + 4U) {
			i915_mmio_record.run_length++;
		} else {
			i915_mmio_record.run_first = i915_mmio_record.used;
			i915_mmio_record.run_length = 1U;
		}
	} else {
		i915_mmio_record.run_first = i915_mmio_record.used;
		i915_mmio_record.run_length = 1U;
	}

	/* Keeps the write as an entry of its own. */
	entry = &i915_mmio_record.entry[i915_mmio_record.used];
	entry->offset = offset;
	entry->value = value;
	entry->count = 1U;
	entry->mark = NULL;
	i915_mmio_record.used++;

	/* A run that reached its threshold becomes one entry. */
	if (i915_mmio_record.run_length == I915_MMIO_RECORD_RUN)
		i915_mmio_record_fold_locked();
}

/* Folds the last I915_MMIO_RECORD_RUN writes, to consecutive registers, into one run entry. */
static void
i915_mmio_record_fold_locked(void)
{
	struct i915_mmio_record_entry *first;
	uint32_t sum;
	unsigned i;

	/* Sums the values of the run. */
	first = &i915_mmio_record.entry[i915_mmio_record.run_first];
	sum = 0U;
	for (i = 0U; i < I915_MMIO_RECORD_RUN; i++)
		sum += i915_mmio_record.entry[i915_mmio_record.run_first + i].value;

	/* The first entry stands for the whole run; the others are given back. */
	first->count = I915_MMIO_RECORD_RUN;
	first->value = sum;
	i915_mmio_record.used = i915_mmio_record.run_first + 1U;
}

/* Closes the record; early tells that it closed itself before it was reported. */
static void
i915_mmio_record_close_locked(
	int early)
{
	/* No write is handed over from here on. */
	drv_i915_mmio_record_on = 0;
	i915_mmio_record.closed_early = early;
}

/* Mixes a register and a value into one word for the digests. */
static uint32_t
i915_mmio_record_mix(
	uint32_t offset,
	uint32_t value)
{
	uint32_t mixed;

	/* Spreads the register over the word, then mixes the value in. */
	mixed = offset * 0x9e3779b1U;
	mixed ^= value;
	mixed ^= mixed >> 16;
	mixed *= 0x85ebca6bU;
	mixed ^= mixed >> 13;

	/* Succeeded: the mixed word. */
	return mixed;
}
