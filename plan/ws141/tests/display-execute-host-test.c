/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* The production executor runs against an observable kernel I/O model. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <kern/dcache.h>
#include <kern/device-io.h>
#include <kern/pmem.h>
#include <kern/irq.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"
#include "drivers/gpu/bcm2711/display-program.h"
#include "drivers/platform/rpi4/rpi4-firmware.h"

/* One model owns the complete register space for each private window role. */
static uint32_t test_memory[BCM2711_REGION_COUNT][8192];

/* Window mappings are retained throughout each production-executor invocation. */
static struct bcm2711_window test_windows[BCM2711_REGION_COUNT];

/* Immutable mode and framebuffer inputs live for all scenarios. */
static struct bcm2711_display_mode test_mode;
static struct drv_bcm2711_boot_screen test_screen;

/* The actual generator owns these commands until the executor returns. */
static struct bcm2711_display_program test_program;

/* The actual IRQ service owns this persistent display state in each scenario. */
static struct bcm2711_display test_display;

/* Kernel IRQ delivery is modeled independently for HVS and both PV lines. */
static bool test_open_irq[3];

/* A small token stands for mapped framebuffer storage; cache stubs inspect it. */
static uint8_t test_framebuffer;

/* Observable effects, errors and modeled hardware completion for one scenario. */
static unsigned test_failures;
static unsigned test_writes;
static unsigned test_notified;
static unsigned test_cleaned;
static unsigned test_recenter;
static unsigned test_delays;
static bool test_refuse_notification;
static bool test_changed_framebuffer;
static bool test_zero_hsm;
static bool test_stall_frame;
static uint32_t test_fail_clock;
static uint32_t test_rates[15];
static uint32_t test_core_at_video;
static uint32_t test_base;
static uint32_t test_timing;

static void check(bool condition, const char *what);
static void setup(uint32_t port);
static uint32_t region_of(const volatile void *address, uint32_t *offset);
static uint32_t locate_clock(uint32_t id);
static void deliver_timing(void);

/*
 * Verifies hardware-visible ordering and that failures stop the real executor.
 * This model does not prove electrical timing, firmware lifetime or GPU output.
 */
int
main(
	void)
{
	int error;
	bool handled;

	/* A successful HDMI0 scanout must wait before lowering its setup clock. */
	setup(0);
	error = bcm2711_display_program_execute(&test_program, test_windows, &test_screen, &test_display);
	check(error == 0, "HDMI0 execution");
	check(test_notified == 1 && test_cleaned == 1, "notification and cache clean");
	check(test_core_at_video == 500000000, "setup core rate retained through video enable");
	check(test_rates[4] == 137600000, "core rate reduced after frame/list adoption");
	check(test_rates[13] == 149985000 && test_rates[14] == 75000000, "HSM and BVB rates");
	check(test_recenter == 2 && test_delays >= 1000, "double FIFO recenter and delay");
	check(test_memory[BCM2711_REGION_HVS][0x30 / 4] == 43, "current list observed");
	check(test_memory[BCM2711_REGION_HDMI1 + BCM2711_ENCODER_SHARED][0x48 / 4] == 0, "other output not enabled");
	check(test_display.first_frame && test_display.timing_irq[0].count == 1, "vblank serviced before adoption completes");
	check((test_memory[BCM2711_REGION_HVS][0] & 0x200U) != 0, "underrun enabled after adoption");

	/* An owned underrun must be masked and quieted by the production service. */
	test_memory[BCM2711_REGION_HVS][0x04 / 4] = 0x200;
	handled = test_display.compositor_irq.service(test_display.compositor_irq.owner);
	check(handled && test_display.underruns == 1, "owned HVS underrun serviced");
	check((test_memory[BCM2711_REGION_HVS][0] & 0x200U) == 0, "reported underrun masked");
	check(test_memory[BCM2711_REGION_HVS][0x04 / 4] == 0, "HVS source acknowledged");
	handled = test_display.compositor_irq.service(test_display.compositor_irq.owner);
	check(!handled && test_display.underruns == 1, "unowned HVS event ignored");

	/* The same executor respects HDMI1's separate shared-register offset. */
	setup(1);
	error = bcm2711_display_program_execute(&test_program, test_windows, &test_screen, &test_display);
	check(error == 0, "HDMI1 execution");
	check(test_rates[4] == 148500000, "HDMI1 core rate");
	check((test_memory[BCM2711_REGION_HDMI1 + BCM2711_ENCODER_SHARED][0x48 / 4] & 0x80000000U) != 0, "HDMI1 video offset");
	check(test_memory[BCM2711_REGION_HDMI0 + BCM2711_ENCODER_SHARED][0x44 / 4] == 0, "HDMI0 remains disabled");

	/* A stale firmware list cannot complete a newly armed display commit. */
	test_display.first_frame = false;
	test_memory[BCM2711_REGION_HVS][0x30 / 4] = 3;
	test_memory[test_timing][0x28 / 4] = 0x80;
	deliver_timing();
	check(!test_display.first_frame, "old list does not complete the new frame");
	check(test_memory[test_timing][0x28 / 4] == 0, "old frame source acknowledged");

	/* A refused ownership transfer must leave all display registers untouched. */
	setup(0);
	test_refuse_notification = true;
	error = bcm2711_display_program_execute(&test_program, test_windows, &test_screen, &test_display);
	check(error == ENOTSUP && test_writes == 0 && test_program.failure == 0, "notification refusal stops all register writes");

	/* A visible framebuffer change after notification stops HVS initialization. */
	setup(0);
	test_changed_framebuffer = true;
	error = bcm2711_display_program_execute(&test_program, test_windows, &test_screen, &test_display);
	check(error == EIO && test_writes == 0, "changed framebuffer stops all register writes");

	/* HSM at zero must prevent the first access to either encoder's registers. */
	setup(0);
	test_zero_hsm = true;
	error = bcm2711_display_program_execute(&test_program, test_windows, &test_screen, &test_display);
	check(error == ENODEV, "zero HSM refusal");
	check(test_memory[BCM2711_REGION_HDMI0 + BCM2711_ENCODER_DVP][0xbc / 4] == 0, "encoder unread/written after zero HSM");
	check(test_program.failure == locate_clock(13), "failure retained at HSM operation");

	/* A failed BVB rate request cannot reach PHY, PV video or HDMI video. */
	setup(0);
	test_fail_clock = 14;
	error = bcm2711_display_program_execute(&test_program, test_windows, &test_screen, &test_display);
	check(error == EIO, "BVB clock failure");
	check(test_memory[test_base + BCM2711_ENCODER_PHY][0x28 / 4] == 0, "no PHY after clock failure");
	check(test_memory[test_timing][0x04 / 4] == 0, "no pixel flow after clock failure");
	check(test_memory[test_base + BCM2711_ENCODER_SHARED][0x44 / 4] == 0, "no video after clock failure");

	/* A frame timeout must leave the temporary setup rate in place. */
	setup(0);
	test_stall_frame = true;
	error = bcm2711_display_program_execute(&test_program, test_windows, &test_screen, &test_display);
	check(error == ETIMEDOUT, "frame completion timeout");
	check(test_rates[4] == 500000000, "no premature core-rate reduction");

	/* Reports failures without claiming a physical scanout result. */
	if (test_failures != 0) {
		printf("display-execute-host-test FAIL %u\n", test_failures);
		return 1;
	}

	/* Succeeded: the real executor honored ordering and failure boundaries. */
	puts("display-execute-host-test PASS");
	return 0;
}

/*
 * Models the firmware transport, checking the exact notification wire shape.
 * All requests are observable and can be refused at a chosen dependency.
 */
int
drv_rpi4_firmware_property(
	uint32_t tag,
	uint32_t *values,
	unsigned request_count,
	unsigned capacity,
	uint32_t *answered)
{
	/* The ownership notification carries no invented value word. */
	if (tag == 0x00030066U) {
		check(values == NULL && request_count == 0 && capacity == 0, "zero-byte display-done wire shape");
		test_notified++;
		if (test_refuse_notification)
			return ENOTSUP;
		*answered = 0;
		return 0;
	}

	/* Clock-provider requests always carry id, value and disable-turbo words. */
	check(request_count == 3 && capacity == 3 && values[2] == 0, "clock-provider request shape");
	if (tag == 0x00038002U) {
		/* A chosen clock dependency can fail before later pipeline stages. */
		if (values[0] == test_fail_clock)
			return EIO;
		test_rates[values[0]] = values[1];
	} else {
		/* Enable requests are the only other provider operation. */
		check(tag == 0x00038001U && values[1] == 1, "clock enable request");
	}

	/* Reports the complete provider response identity. */
	*answered = 12;
	return 0;
}

/*
 * Models the two read-only framebuffer questions after ownership transfer.
 */
int
bcm2711_firmware_get(
	uint32_t tag,
	uint32_t *values,
	unsigned request_count,
	unsigned answer_words)
{
	/* These questions follow the notification and carry no request values. */
	check(test_notified == 1 && request_count == 0, "framebuffer check follows notification");
	if (tag == 0x00040003U) {
		/* The changed-geometry scenario must not permit HVS initialization. */
		check(answer_words == 2, "framebuffer-size response capacity");
		values[0] = 1920;
		if (test_changed_framebuffer)
			values[0] = 1280;
		values[1] = 1080;
	} else {
		/* The unchanged pitch completes the visible-geometry checks. */
		check(tag == 0x00040008U && answer_words == 1, "framebuffer-pitch question");
		values[0] = 7680;
	}

	/* Succeeded: a complete modeled answer was supplied. */
	return 0;
}

/*
 * Reports modeled clock rates, with a zero-HSM fault available after prepare.
 */
int
bcm2711_clock_hz(
	uint32_t clock_id,
	uint32_t *hz)
{
	/* A zero-rate HSM must be treated as a hard access prerequisite failure. */
	*hz = test_rates[clock_id];
	if (clock_id == 13 && test_zero_hsm)
		*hz = 0;

	/* Succeeded: the rate itself determines whether hardware may be accessed. */
	return 0;
}

/*
 * Reads an observed register and models the hardware's completion bits.
 */
uint32_t
kern_mmio_read32(
	const volatile void *address)
{
	uint32_t region;
	uint32_t offset;
	uint32_t value;

	/* Resolves an address only within the test-owned mapped windows. */
	region = region_of(address, &offset);
	value = test_memory[region][offset / 4];
	if (region == test_base + BCM2711_ENCODER_CORE && offset == 0x74 && test_recenter == 2) {
		/* FIFO recenter completion is observed after the second trigger. */
		value |= 0x4000;
	}

	/* Reports the modeled hardware observation. */
	return value;
}

/*
 * Records an ordered write and models only the relevant hardware side effects.
 */
void
kern_mmio_write32(
	volatile void *address,
	uint32_t value)
{
	uint32_t region;
	uint32_t offset;
	uint32_t previous;

	/* Every write must occur after the firmware ownership notification. */
	check(test_notified == 1, "register write follows notification");
	region = region_of(address, &offset);
	test_writes++;
	previous = test_memory[region][offset / 4];
	test_memory[region][offset / 4] = value;
	if (region == BCM2711_REGION_HVS && offset == 0x04)
		test_memory[region][offset / 4] = previous & ~value;
	if (region == test_base + BCM2711_ENCODER_CORE) {
		/* The scheduler and packet status follow their modeled enable requests. */
		if (offset == 0xe0 && (value & 1U) != 0)
			test_memory[region][offset / 4] |= 2;
		if (offset == 0xe0 && (value & 1U) == 0)
			test_memory[region][offset / 4] &= ~2U;
		if (offset == 0xbc)
			test_memory[region][0xc4 / 4] = value & 0xffffU;
		if (offset == 0x74 && (value & 0x40U) != 0)
			test_recenter++;
	}

	/* PV W1C clears old status; enabled flow eventually adopts the new list. */
	if (region == test_timing && offset == 0x28)
		test_memory[region][offset / 4] = 0;
	if (region == test_timing && offset == 0x04 && (value & 1U) != 0 && !test_stall_frame) {
		/* Hardware frame adoption precedes the owned vblank service. */
		test_memory[BCM2711_REGION_HVS][0x30 / 4] = test_memory[BCM2711_REGION_HVS][0x20 / 4];
		test_memory[test_timing][0x28 / 4] = 0x80;
		deliver_timing();
	}

	/* Video must follow pixel flow and preserve the temporary core rate. */
	if (region == test_base + BCM2711_ENCODER_SHARED && offset == 0x44 + (test_timing - BCM2711_REGION_PV0) * 4 &&
	    (value & 0x80000000U) != 0) {
		check((test_memory[test_timing][0x04 / 4] & 1U) != 0, "pixel flow precedes HDMI video");
		test_core_at_video = test_rates[4];
	}
}

/*
 * Models the boot-time wait without delaying the host process.
 */
void
kern_usleep_range(
	unsigned min_us,
	unsigned max_us)
{
	/* The executor requests bounded, exact waits in this path. */
	check(min_us == max_us, "bounded delay request");
	test_delays += min_us;
}

/*
 * Supplies the model's framebuffer mapping without dereferencing physical RAM.
 */
void *
kern_pmem_to_kernel(
	hal_physaddr_t address)
{
	/* Only the boot framebuffer may be cleaned for the generated primary plane. */
	check(address == test_screen.physical, "framebuffer mapping address");

	/* Returns storage owned for the complete test scenario. */
	return &test_framebuffer;
}

/*
 * Checks that cache cleaning precedes the first primary SRAM word.
 */
void
kern_dcache_clean_range(
	const void *address,
	size_t size)
{
	/* The complete buffer must become visible before the HVS reads it. */
	check(address == &test_framebuffer && size == test_screen.size, "framebuffer cache-clean span");
	check(test_memory[BCM2711_REGION_HVS][0x40ac / 4] == 0, "cache clean precedes list write");
	test_cleaned++;
}

/*
 * Models the explicit device-write barrier following framebuffer cache clean.
 */
void
kern_io_write_barrier(
	void)
{
	/* The barrier must follow the one complete framebuffer clean. */
	check(test_cleaned == 1, "write barrier follows cache clean");
}

/*
 * Opens only a registered model line after the real initialization command.
 */
void
kern_irq_unmask(
	int irq)
{
	/* The generated program opens HVS before HDMI and PV only after source clear. */
	check(irq >= 0 && irq < 3 && test_notified == 1, "serviced IRQ opening");
	test_open_irq[irq] = true;
}

/*
 * Masks a modeled line after a failed boot attempt.
 */
void
kern_irq_mask(
	int irq)
{
	/* Persistent source owners survive the failure while delivery stops. */
	check(irq >= 0 && irq < 3, "IRQ mask identity");
	test_open_irq[irq] = false;
}

/*
 * Models atomic local exclusion while stale frame status is discarded.
 */
bool
kern_irq_disable(
	void)
{
	/* The model is serialized; the production code still requests exclusion. */
	return true;
}

/*
 * Restores local delivery after the production frame-arm critical section.
 */
void
kern_irq_enable(
	void)
{
	/* No asynchronous host thread delivers interrupts in this model. */
}

/* Records one failed observation without abandoning the remaining scenarios. */
static void
check(
	bool condition,
	const char *what)
{
	/* Accumulates each failure and its independently chosen explanation. */
	if (!condition) {
		printf("FAIL %s\n", what);
		test_failures++;
	}
}

/* Resets the model and prepares a known DVI-mode program for one selected port. */
static void
setup(
	uint32_t port)
{
	uint32_t index;
	int error;

	/* Each independent scenario owns a fresh register image and failure state. */
	memset(test_memory, 0, sizeof(test_memory));
	memset(&test_mode, 0, sizeof(test_mode));
	memset(&test_screen, 0, sizeof(test_screen));
	memset(test_rates, 0, sizeof(test_rates));
	memset(&test_display, 0, sizeof(test_display));
	memset(test_open_irq, 0, sizeof(test_open_irq));
	test_writes = 0;
	test_notified = 0;
	test_cleaned = 0;
	test_recenter = 0;
	test_delays = 0;
	test_refuse_notification = false;
	test_changed_framebuffer = false;
	test_zero_hsm = false;
	test_stall_frame = false;
	test_fail_clock = 0;
	test_core_at_video = 0;
	test_base = BCM2711_REGION_HDMI0 + port * BCM2711_ENCODER_REGIONS;
	test_timing = BCM2711_REGION_PV0 + port;
	test_rates[4] = 250000000;
	test_rates[13] = 120000000;
	test_rates[14] = 75000000;
	for (index = 0; index < BCM2711_REGION_COUNT; index++) {
		/* Maps only storage owned by this test model. */
		test_windows[index].mapped = (volatile uint8_t *)test_memory[index];
		test_windows[index].physical = 0;
		test_windows[index].size = sizeof(test_memory[index]);
	}

	/* Publishes the same mapped registers to the production device-source handlers. */
	test_display.compositor = test_windows[BCM2711_REGION_HVS];
	test_display.compositor_irq.irq = 0;
	test_display.compositor_irq.registered = true;
	test_display.port = port;
	test_display.channel = 0;
	for (index = 0; index < BCM2711_TIMING_COUNT; index++) {
		/* Each modeled PV line has a distinct persistent source owner. */
		test_display.timing[index] = test_windows[BCM2711_REGION_PV0 + index];
		test_display.timing_irq[index].irq = (int)index + 1;
		test_display.timing_irq[index].registered = true;
	}

	/* Installs the production source service before running any hardware command. */
	error = bcm2711_display_irq_prepare(&test_display);
	check(error == 0, "production IRQ source preparation");

	/* Uses known timings with no AVI/SCDC requirements to isolate execution. */
	test_mode.width = 1920;
	test_mode.height = 1080;
	test_mode.hfront = 88;
	test_mode.hsync = 44;
	test_mode.hback = 148;
	test_mode.vfront = 4;
	test_mode.vsync = 5;
	test_mode.vback = 36;
	test_mode.tmds_hz = 148500000;
	test_mode.hpositive = true;
	test_mode.vpositive = true;
	test_screen.physical = 0x01000000;
	test_screen.size = 7680U * 1080U;
	test_screen.width = 1920;
	test_screen.height = 1080;
	test_screen.pitch = 7680;
	error = bcm2711_display_program_prepare(&test_program, &test_mode, &test_screen, port, 2, 600000000);
	check(error == 0, "model program preparation");
}

/* Resolves an I/O address with defined uintptr arithmetic across model arrays. */
static uint32_t
region_of(
	const volatile void *address,
	uint32_t *offset)
{
	uintptr_t actual;
	uintptr_t first;
	uint32_t region;

	/* Only a mapped region can supply an observable hardware address. */
	actual = (uintptr_t)address;
	for (region = 0; region < BCM2711_REGION_COUNT; region++) {
		/* Bounds before subtracting, avoiding unrelated-pointer subtraction. */
		first = (uintptr_t)test_memory[region];
		if (actual < first || actual - first >= sizeof(test_memory[region]))
			continue;
		*offset = (uint32_t)(actual - first);

		/* Returns the exact mapped region. */
		return region;
	}

	/* Retains an explicit model failure if production used an unmapped address. */
	check(false, "unmapped register access");
	*offset = 0;
	return 0;
}

/* Finds the actual clock-on command index for failure-boundary verification. */
static uint32_t
locate_clock(
	uint32_t id)
{
	uint32_t index;

	/* Searches the production program rather than assuming an index. */
	for (index = 0; index < test_program.count; index++) {
		/* The first preparation of this clock is its zero-rate access guard. */
		if (test_program.commands[index].operation == BCM2711_DISPLAY_CLOCK_ON &&
		    test_program.commands[index].region == id)
			return index;
	}

	/* No command means the failure observation cannot match a valid index. */
	return test_program.count;
}

/* Delivers one modeled PV event through the actual device-source service. */
static void
deliver_timing(
	void)
{
	struct bcm2711_irq_line *line;
	bool handled;

	/* The GIC must be open before a device source can retire an event. */
	line = &test_display.timing_irq[test_display.port];
	check(test_open_irq[line->irq], "PV IRQ opened before video flow");
	handled = line->service(line->owner);
	if (handled)
		line->count++;
}
