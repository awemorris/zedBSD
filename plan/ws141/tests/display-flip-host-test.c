/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Real flip and source services run against a deferred-adoption MMIO model. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <kern/clock.h>
#include <kern/dcache.h>
#include <kern/device-io.h>
#include <kern/pmem.h>
#include <kern/irq.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"

/* Persistent display mappings and owner survive every IRQ in one scenario. */
static struct bcm2711_display test_display;

/* SRAM and register windows model HVS and both PV sources without real DMA. */
static uint32_t test_hvs[8192];
static uint32_t test_pv[2][32];

/* Three disjoint physical buffers share geometry but have separate ownership. */
static struct drv_bcm2711_boot_screen test_frames[3];
static uint8_t test_mapping;

/* Effects are checked at publication, while ticks control real source delivery. */
static unsigned test_failures;
static unsigned test_writes;
static unsigned test_cleaned;
static unsigned test_barriers;
static unsigned test_ticks;
static bool test_stall;
static bool test_probe_busy;
static uint32_t test_expected_physical;

static void check(bool condition, const char *what);
static void setup(uint32_t port, uint32_t format);
static void deliver(uint32_t port, uint32_t current);
static void normal_flips(uint32_t port, uint32_t format);
static void uncertain_flips(void);

/*
 * Checks publication, adoption and retained-buffer lifetime through real code.
 * Electrical scanout, firmware RAM lifetime and SMP lock ordering remain untested.
 */
int
main(
	void)
{
	struct bcm2711_flip_status status;
	struct drv_bcm2711_boot_screen bad;
	unsigned before;
	int error;

	/* Runs both physical port choices and firmware RGB byte orders. */
	normal_flips(0, 0);
	normal_flips(1, 1);
	uncertain_flips();

	/* Rejects invalid DMA geometry before any hardware publication. */
	setup(0, 0);
	error = bcm2711_display_flip_attach(&test_display);
	check(error == 0, "attach before refusal cases");
	before = test_writes;
	bad = test_frames[0];
	bad.physical = 0x40000000ULL;
	error = bcm2711_display_flip_present(&test_display, &bad);
	check(error == EINVAL && test_writes == before, "one-GiB DMA limit");
	bad = test_frames[0];
	bad.height++;
	error = bcm2711_display_flip_present(&test_display, &bad);
	check(error == EINVAL && test_writes == before, "mode cannot change");
	bad = test_frames[0];
	bad.size--;
	error = bcm2711_display_flip_present(&test_display, &bad);
	check(error == EINVAL && test_writes == before, "last DMA row bounded");
	bad = test_frames[0];
	bad.format = 2;
	error = bcm2711_display_flip_present(&test_display, &bad);
	check(error == ENOTSUP && test_writes == before, "unknown RGB encoding refused");
	bad = test_display.screen;
	error = bcm2711_display_flip_present(&test_display, &bad);
	check(error == EINVAL && test_writes == before, "console overlap refused");

	/* Detects foreign channel ownership without overwriting its live SRAM. */
	test_hvs[0x50 / 4] = 0x80000000U;
	error = bcm2711_display_flip_present(&test_display, &test_frames[0]);
	check(error == EBUSY && test_writes == before, "foreign enabled channel untouched");
	bcm2711_display_flip_snapshot(&test_display, &status);
	check(status.uncertain && status.retained_mask == 0, "prepublication error never holds candidate");
	error = bcm2711_display_flip_restore(&test_display);
	check(error == EBUSY && test_writes == before, "recovery cannot reset foreign channel");
	test_hvs[0x50 / 4] = 0;
	test_hvs[0x30 / 4] = 99;
	error = bcm2711_display_flip_restore(&test_display);
	check(error == EBUSY && test_writes == before, "foreign current list untouched");

	/* Refuses attaching a component before the real initial scanout has succeeded. */
	setup(0, 0);
	test_display.scanout_started = false;
	error = bcm2711_display_flip_attach(&test_display);
	check(error == ENODEV && test_writes == 0, "R0 prerequisite preserved");

	/* Reports software outcomes without implying physical display acceptance. */
	if (test_failures != 0) {
		printf("display-flip-host-test FAIL %u\n", test_failures);
		return 1;
	}

	/* Succeeded: the production component honors all modeled lifetime boundaries. */
	puts("display-flip-host-test PASS");
	return 0;
}

/*
 * Models accepted provider requests outside the IRQ guard during exclusive admission.
 */
int
drv_rpi4_firmware_property(
	uint32_t tag,
	uint32_t *values,
	unsigned request_count,
	unsigned capacity,
	uint32_t *answered)
{
	/* Native list publication owns BUSY, but firmware requests must not hold a spinlock. */
	assert(test_display.flip.busy && test_display.flip.guard.held.value == 0);
	assert(tag == 0x00038002U && request_count == 3 && capacity == 3);
	assert(values[0] == 4 && values[2] == 0);
	*answered = 8;

	/* Succeeded: the modeled provider accepted the requested floor. */
	return 0;
}

/*
 * Models ordered reads from persistent register windows.
 */
uint32_t
kern_mmio_read32(
	const volatile void *address)
{
	/* Mapped pointers identify ordinary model storage rather than device memory. */
	return *(const volatile uint32_t *)address;
}

/*
 * Models W1C sources and observes complete lists before next-pointer publication.
 */
void
kern_mmio_write32(
	volatile void *address,
	uint32_t word)
{
	unsigned port;
	uint32_t expected_order;
	uint32_t *list;

	/* Tracks writes and quiets HVS channel IRQ status using W1C semantics. */
	test_writes++;
	if (address == &test_hvs[0x04 / 4]) {
		test_hvs[0x04 / 4] &= ~word;
		return;
	}

	/* Retires only the addressed PV source. */
	for (port = 0; port < 2; port++) {
		/* A write to one output must not acknowledge the other output's IRQ. */
		if (address == &test_pv[port][0x28 / 4]) {
			test_pv[port][0x28 / 4] &= ~word;
			return;
		}
	}

	/* Validates the hardware-visible plane with literal independent word values. */
	if (address == &test_hvs[0x20 / 4]) {
		/* Publication occurs under lock after cache clean, W1C and state arming. */
		check(test_display.flip.guard.held.value == 1, "publication guarded");
		check(test_display.flip.pending_list == word, "completion armed before pointer");
		check(test_cleaned != 0 && test_barriers >= 2, "cache/barrier before publication");
		check(test_pv[test_display.port][0x28 / 4] == 0, "stale PV status discarded");
		if (word != 43) {
			/* Context words and terminator must already exist in the inactive slot. */
			check(word == 64 || word == 80, "dedicated SRAM slot");
			check(test_hvs[0x30 / 4] != word, "active list never overwritten");
			list = &test_hvs[0x4000 / 4 + word];
			expected_order = 0x4800d807U;
			if (test_display.screen.format == 1)
				expected_order = 0x4800f807U;
			check(list[0] == expected_order && list[1] == 0, "literal RGB plane header");
			check(list[2] == 0x4000fff0U && list[3] == 0x04380780U, "literal dimensions/alpha");
			check(list[4] == 0xc0c0c0c0U && list[6] == 0xc0c0c0c0U, "fresh writable contexts");
			check(list[5] == (0xc0000000U | test_expected_physical), "candidate DMA pointer");
			check(list[7] == 7680 && list[8] == 0x80000000U, "pitch and END before pointer");
		}
	}

	/* Pointer publication does not advance current until a modeled frame starts. */
	*(volatile uint32_t *)address = word;
}

/*
 * Models deferred hardware adoption while ensuring waits never hold an IRQ guard.
 */
void
kern_usleep_range(
	unsigned min_us,
	unsigned max_us)
{
	/* Advances only after the caller has left its publication critical section. */
	assert(min_us == 10 && max_us == 10);
	assert(test_display.flip.guard.held.value == 0);
	test_ticks++;
	if (test_stall)
		return;

	/* Old-list and wrong-port interrupts cannot complete the pending operation. */
	if (test_ticks == 1) {
		/* The selected PV has a genuine event but HVS still uses the preceding list. */
		deliver(test_display.port, test_hvs[0x30 / 4]);
		check(!test_display.flip.completed, "old current list cannot complete flip");
	} else if (test_ticks == 2) {
		/* Matching HVS adoption on another PV's event is insufficient. */
		deliver(1U - test_display.port, test_hvs[0x20 / 4]);
		check(!test_display.flip.completed, "wrong-port IRQ cannot complete flip");
	} else {
		/* The selected source and adopted current pointer jointly complete the call. */
		deliver(test_display.port, test_hvs[0x20 / 4]);
	}
}

/*
 * Supplies a stable token for all bounded caller-owned physical mappings.
 */
void *
kern_pmem_to_kernel(
	hal_physaddr_t physical)
{
	/* Physical descriptors are validated by production code, not this mapping stub. */
	(void)physical;

	/* Succeeded: modeled CPU access remains valid throughout cache preparation. */
	return &test_mapping;
}

/*
 * Observes cache publication and optionally reenters the real caller interface.
 */
void
kern_dcache_clean_range(
	const void *address,
	size_t length)
{
	int error;

	/* Cache cleaning must occur outside the IRQ guard and cover complete rows. */
	assert(address == &test_mapping);
	assert(length == 8294400U);
	assert(test_display.flip.guard.held.value == 0);
	test_cleaned++;
	if (test_probe_busy) {
		/* Concurrent ownership attempts cannot override the reserved operation. */
		test_probe_busy = false;
		error = bcm2711_display_flip_present(&test_display, &test_frames[2]);
		check(error == EBUSY, "present admission serialized during cache preparation");
		error = bcm2711_display_flip_restore(&test_display);
		check(error == EBUSY, "restore admission serialized during cache preparation");
	}
}

/*
 * Observes explicit ordering boundaries without simulating ARM bus behavior.
 */
void
kern_io_write_barrier(
	void)
{
	/* Counters let next-pointer publication verify its required predecessors. */
	test_barriers++;
}

/*
 * Supplies inert line masking because real source callbacks are invoked directly.
 */
void
kern_irq_unmask(
	int irq)
{
	/* The model keeps all persistent source owners alive regardless of routing. */
	(void)irq;
}

/*
 * Supplies inert line masking for the executor's shared IRQ helper references.
 */
void
kern_irq_mask(
	int irq)
{
	/* Device-source W1C is modeled separately from interrupt-controller masking. */
	(void)irq;
}

/* Records one failure without hiding subsequent independent lifetime assertions. */
static void
check(
	bool condition,
	const char *what)
{
	/* Retains the first and later failures for a bounded, readable host report. */
	if (!condition) {
		printf("FAIL %s\n", what);
		test_failures++;
	}
}

/* Prepares a running R0 fixture with a preserved console list and registered IRQs. */
static void
setup(
	uint32_t port,
	uint32_t format)
{
	unsigned slot;
	int error;

	/* Clears one stopped model before attaching any source callbacks. */
	memset(&test_display, 0, sizeof(test_display));
	memset(test_hvs, 0, sizeof(test_hvs));
	memset(test_pv, 0, sizeof(test_pv));
	test_display.compositor.mapped = (volatile uint8_t *)test_hvs;
	test_display.compositor.size = sizeof(test_hvs);
	test_display.compositor_irq.registered = true;
	test_display.port = port;
	test_display.channel = 0;
	test_display.screen.physical = 0x01000000U;
	test_display.screen.size = 8294400U;
	test_display.screen.width = 1920;
	test_display.screen.height = 1080;
	test_display.screen.pitch = 7680;
	test_display.screen.format = format;
	test_display.scanout_started = true;
	test_display.max_core_hz = 500000000;
	test_display.console_core_hz = 137600000;
	test_display.refresh_millihz = 60000;
	test_display.adoption_armed = true;
	test_display.first_frame = true;
	for (slot = 0; slot < 2; slot++) {
		/* Each PV owns its own IRQ source and complete mapped register window. */
		test_display.timing[slot].mapped = (volatile uint8_t *)test_pv[slot];
		test_display.timing[slot].size = sizeof(test_pv[slot]);
		test_display.timing_irq[slot].registered = true;
	}

	/* Gives the production service the exact sole channel-zero R0 register state. */
	test_hvs[0x40 / 4] = 0x87800438U;
	test_hvs[0x14 / 4] = 0xc0000000U;
	test_hvs[0x18 / 4] = 0xc0000000U;
	test_hvs[(0x18U - port * 4U) / 4] = 0;
	test_hvs[0x20 / 4] = 43;
	test_hvs[0x30 / 4] = 43;
	test_hvs[0x4000 / 4 + 43] = 0x11223344U;
	test_hvs[0x4000 / 4 + 51] = 0x80000000U;
	test_pv[port][0] = 1;
	test_pv[port][1] = 1;
	error = bcm2711_display_irq_prepare(&test_display);
	check(error == 0, "real source owners prepared");

	/* Creates disjoint borrowed buffers with the established RGB32 geometry. */
	for (slot = 0; slot < 3; slot++) {
		/* Each buffer starts sixteen MiB after the preceding physical allocation. */
		test_frames[slot] = test_display.screen;
		test_frames[slot].physical = 0x02000000U + slot * 0x01000000U;
	}

	/* Resets per-operation observations while retaining the total failure count. */
	test_writes = 0;
	test_cleaned = 0;
	test_barriers = 0;
	test_ticks = 0;
	test_stall = false;
	test_probe_busy = false;
	test_expected_physical = (uint32_t)test_frames[0].physical;
}

/* Delivers one real PV service against a specified hardware current-list value. */
static void
deliver(
	uint32_t port,
	uint32_t current)
{
	bool handled;

	/* Hardware adoption is independent of the source whose interrupt is delivered. */
	test_hvs[0x30 / 4] = current;
	test_pv[port][0x28 / 4] = 0x80;
	handled = test_display.timing_irq[port].service(test_display.timing_irq[port].owner);
	check(handled && test_pv[port][0x28 / 4] == 0, "actual PV source acknowledged");
}

/* Exercises alternating SRAM and ordinary buffer retirement on both outputs. */
static void
normal_flips(
	uint32_t port,
	uint32_t format)
{
	struct bcm2711_flip_status status;
	unsigned index;
	unsigned before;
	int error;

	/* Attachment is read-only and publishes no unverified boot hardware changes. */
	setup(port, format);
	error = bcm2711_display_flip_attach(&test_display);
	check(error == 0 && test_writes == 0, "read-only attach");
	error = bcm2711_display_flip_attach(&test_display);
	check(error == EBUSY && test_writes == 0, "live owner cannot be reattached");
	test_probe_busy = true;
	for (index = 0; index < 3; index++) {
		/* Each complete call must adopt its candidate and retire only the previous one. */
		test_ticks = 0;
		test_expected_physical = (uint32_t)test_frames[index].physical;
		error = bcm2711_display_flip_present(&test_display, &test_frames[index]);
		check(error == 0, "synchronous present");
		bcm2711_display_flip_snapshot(&test_display, &status);
		check(!status.busy && !status.uncertain, "normal completion status");
		check(status.retained_mask == (1U << (index % 2U)), "previous buffer retired after adoption");
		check(status.frames[index % 2U].physical == test_frames[index].physical, "borrowed frame identity");
	}

	/* Selected old/new frames count, while other-port events do not. */
	check(status.frame_sequence == 6, "selected-source frame sequence");

	/* Submitting the currently retained bytes is refused before any list write. */
	before = test_writes;
	error = bcm2711_display_flip_present(&test_display, &test_frames[2]);
	check(error == EINVAL && test_writes == before, "active-buffer overlap refused");
	check(test_hvs[0x4000 / 4 + 43] == 0x11223344U, "console list remains intact");
	test_ticks = 10;
	test_probe_busy = true;
	error = bcm2711_display_flip_restore(&test_display);
	check(error == 0, "console restored through fresh selected frame");
	bcm2711_display_flip_snapshot(&test_display, &status);
	check(status.retained_mask == 0 && status.active_list == 43, "restore retires all borrowed buffers");
}

/* Exercises timeout, late adoption and repeated restoration without premature free. */
static void
uncertain_flips(
	void)
{
	struct bcm2711_flip_status status;
	unsigned before;
	int error;

	/* Establishes one adopted caller-owned frame before stalling the next flip. */
	setup(0, 0);
	error = bcm2711_display_flip_attach(&test_display);
	check(error == 0, "attach timeout scenario");
	error = bcm2711_display_flip_present(&test_display, &test_frames[0]);
	check(error == 0, "first buffer adopted before timeout");
	test_stall = true;
	test_expected_physical = (uint32_t)test_frames[1].physical;
	error = bcm2711_display_flip_present(&test_display, &test_frames[1]);
	check(error == ETIMEDOUT, "missing adoption times out");
	bcm2711_display_flip_snapshot(&test_display, &status);
	check(status.uncertain && status.retained_mask == 3, "timeout retains old and candidate");

	/* A late selected interrupt cannot silently change the error's retirement policy. */
	deliver(0, 80);
	bcm2711_display_flip_snapshot(&test_display, &status);
	check(status.uncertain && status.retained_mask == 3, "late adoption keeps both holds");
	before = test_writes;
	error = bcm2711_display_flip_present(&test_display, &test_frames[2]);
	check(error == EBUSY && test_writes == before, "ordinary flip refuses uncertain DMA");
	error = bcm2711_display_flip_restore(&test_display);
	check(error == ETIMEDOUT, "stalled restoration times out");
	bcm2711_display_flip_snapshot(&test_display, &status);
	check(status.uncertain && status.retained_mask == 3, "failed restoration never retires holds");

	/* A later successful console commit supplies the missing retirement proof. */
	test_stall = false;
	test_ticks = 10;
	test_probe_busy = true;
	error = bcm2711_display_flip_restore(&test_display);
	check(error == 0, "retry console restoration");
	bcm2711_display_flip_snapshot(&test_display, &status);
	check(!status.uncertain && status.retained_mask == 0, "console adoption releases uncertain buffers");
}
