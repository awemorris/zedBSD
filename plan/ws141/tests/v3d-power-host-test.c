/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Native PM startup/reset runs against the real fixed firmware tree and ordered IO. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"

/* One fixture retains only provider registers, making engine access impossible. */
static uint32_t test_pm[0x114 / 4];
static uint32_t test_legacy[0x24 / 4];
static uint32_t test_bridge[0x24 / 4];
static uint32_t test_events[64];
static unsigned test_event_count;
static bool test_clock_on;
static uint32_t test_clock_rate;
static uint32_t test_stuck;

/* Emulator revision refusal must precede all native provider register access. */
static uint32_t test_revision;
static uint32_t test_reads;
static struct bcm2711_v3d test_engine;

static void fixture(void);

/*
 * Checks provider resolution, exact startup/reset ordering and failed bridge retention.
 * The host fixture supplies no proof of actual domain power or electrical timing.
 */
int
main(
	int argc,
	char **argv)
{
	const uint32_t startup[9] = {101, 201, 100, 300, 101, 401, 411, 500, 101};
	struct drv_fdt tree;
	const uint8_t *property;
	uint8_t *mutable;
	uint8_t *blob;
	uint32_t node;
	uint32_t length;
	FILE *file;
	long bytes;
	size_t read;
	int error;

	/* Loads the separately hash-verified firmware artifact without committing its GPL data. */
	assert(argc == 2);
	file = fopen(argv[1], "rb");
	assert(file != NULL);
	error = fseek(file, 0, SEEK_END);
	assert(error == 0);
	bytes = ftell(file);
	assert(bytes > 0);
	error = fseek(file, 0, SEEK_SET);
	assert(error == 0);
	blob = malloc((size_t)bytes);
	assert(blob != NULL);
	read = fread(blob, 1, (size_t)bytes, file);
	assert(read == (size_t)bytes);
	error = fclose(file);
	assert(error == 0);
	error = drv_fdt_open(&tree, blob, (size_t)bytes);
	assert(error == 0);

	/* Exercises actual FDT lookup, translation and named-window resolution before IO. */
	fixture();
	error = bcm2711_v3d_power_prepare(&tree, &test_engine);
	assert(error == 0 && test_engine.power.prepared && test_event_count == 0);
	assert(test_engine.power.control.physical == 0xfe100000);
	assert(test_engine.power.bridge.physical == 0xfec11000 && test_engine.power.bridge.size == 0x24);
	error = bcm2711_v3d_power_start(&test_engine);
	assert(error == 0 && test_engine.power.ready && test_clock_on);
	assert(test_event_count == 9 && memcmp(test_events, startup, sizeof(startup)) == 0);
	assert(test_clock_rate == 500000000 && test_pm[0x10c / 4] == 0x1060);
	assert(test_bridge[0x08 / 4] == 0 && test_bridge[0x0c / 4] == 0);

	/* Actual native reset stops slave then master before clock-off and PM reset assertion. */
	test_event_count = 0;
	error = bcm2711_v3d_power_reset(&test_engine);
	assert(error == 0 && test_engine.power.ready);
	assert(test_events[0] == 410 && test_events[1] == 400 && test_events[2] == 100);
	assert(test_events[3] == 301 && test_events[4] == 101 && test_events[5] == 201);
	assert(test_events[6] == 100 && test_events[7] == 300 && test_events[8] == 101);
	assert(test_events[9] == 401 && test_events[10] == 411 && test_event_count == 11);

	/* A stuck master's stop ACK restores the slave and supplies no reset-completion proof. */
	test_event_count = 0;
	test_stuck = 0x0c;
	error = bcm2711_v3d_power_reset(&test_engine);
	assert(error == ETIMEDOUT && !test_engine.power.ready);
	assert(test_clock_on && test_pm[0x10c / 4] == 0x1060);
	assert(test_events[0] == 410 && test_events[1] == 400 && test_events[2] == 201);
	assert(test_events[3] == 411 && test_event_count == 4);

	/* A stuck startup master prevents the slave enable and leaves engine registers gated. */
	fixture();
	error = bcm2711_v3d_power_prepare(&tree, &test_engine);
	assert(error == 0);
	test_stuck = 0x0c;
	error = bcm2711_v3d_power_start(&test_engine);
	assert(error == ETIMEDOUT && !test_engine.power.ready);
	assert(test_bridge[0x08 / 4] == 3 && test_clock_rate == 200000000);

	/* An emulator firmware answer prevents even PM identity reads. */
	fixture();
	error = bcm2711_v3d_power_prepare(&tree, &test_engine);
	assert(error == 0 && test_reads == 0);
	test_revision = 0;
	error = bcm2711_v3d_power_start(&test_engine);
	assert(error == ENODEV && test_reads == 0 && test_event_count == 0);

	/* A foreign domain is refused before mapping, clock requests or provider writes. */
	error = bcm2711_fdt_find(&tree, "brcm,2711-v3d", &node);
	assert(error == 0);
	error = drv_fdt_property(&tree, node, "power-domains", &property, &length);
	assert(error == 0 && length == 8);
	mutable = (uint8_t *)property;
	mutable[7] = 2;
	fixture();
	error = bcm2711_v3d_power_prepare(&tree, &test_engine);
	assert(error == ENOTSUP && !test_engine.power.prepared && test_event_count == 0);
	free(blob);

	/* Succeeded: the default provider path obeys modeled ownership and failure boundaries. */
	puts("v3d-power-host-test PASS");
	return 0;
}

/*
 * Maps only the actual provider windows described by the fixed firmware.
 */
int
kern_device_map(
	uint64_t physical,
	size_t bytes,
	unsigned attributes,
	void **mapping)
{
	/* Engine register windows cannot be reached by any preparation or power operation. */
	assert(attributes == KERN_DEVICE_UNCACHED);
	if (physical == 0xfe100000) {
		assert(bytes == sizeof(test_pm));
		*mapping = test_pm;
	} else if (physical == 0xfe00a000) {
		assert(bytes == sizeof(test_legacy));
		*mapping = test_legacy;
	} else {
		assert(physical == 0xfec11000 && bytes == sizeof(test_bridge));
		*mapping = test_bridge;
	}

	/* Succeeded: the CPU fixture represents one permanent provider role. */
	return 0;
}

/*
 * Reads only modeled PM and ASB registers.
 */
uint32_t
kern_mmio_read32(
	const volatile void *address)
{
	/* Any engine register dereference lies outside the fixture and would fail. */
	test_reads++;
	return *(const volatile uint32_t *)address;
}

/*
 * Models password-protected reset and AXI bridge acknowledgement independently.
 */
void
kern_mmio_write32(
	volatile void *address,
	uint32_t value)
{
	uint32_t offset;
	uint32_t event;
	uint32_t saved;

	/* Password and unrelated PM bits are preserved at each actual state transition. */
	assert((value & 0xff000000U) == 0x5a000000);
	value &= 0x00ffffff;
	if (address == &test_pm[0x10c / 4]) {
		assert((value & ~0x40U) == 0x1020);
		event = 301;
		if ((value & 0x40U) != 0)
			event = 300;
		*(volatile uint32_t *)address = value;
	} else {
		/* A bridge change requires the engine clock while opening or stopping traffic. */
		assert(test_clock_on);
		offset = (uint32_t)((volatile uint8_t *)address - (volatile uint8_t *)test_bridge);
		assert(offset == 0x08 || offset == 0x0c);
		event = 400;
		if (offset == 0x08)
			event = 410;
		if ((value & 1U) == 0)
			event++;
		saved = *(volatile uint32_t *)address & 2U;
		value &= ~2U;
		if (offset == test_stuck) {
			value |= saved;
		} else if ((value & 1U) != 0) {
			value |= 2U;
		}

		/* ACK follows the independent hardware model, never a driver inference. */
		*(volatile uint32_t *)address = value;
	}

	/* Captures actual cross-provider ordering for the literal sequence oracle. */
	assert(test_event_count < 64);
	test_events[test_event_count++] = event;
}

/*
 * Models firmware state/rate requests without changing any native bridge register.
 */
int
drv_rpi4_firmware_property(
	uint32_t tag,
	uint32_t *values,
	unsigned request_count,
	unsigned capacity,
	uint32_t *answered)
{
	uint32_t event;

	/* Every provider write has the existing id/value/turbo wire shape. */
	assert(request_count == 3 && capacity == 3 && values[0] == 5 && values[2] == 0);
	if (tag == 0x00038001) {
		test_clock_on = false;
		if (values[1] == 1)
			test_clock_on = true;
		event = 100 + values[1];
	} else {
		assert(tag == 0x00038002 && values[1] == 500000000);
		test_clock_rate = values[1];
		event = 500;
	}

	/* Clock effects are recorded before any later native provider write. */
	assert(test_event_count < 64);
	test_events[test_event_count++] = event;
	*answered = 8;

	/* Succeeded: the independent model accepted exactly this provider request. */
	return 0;
}

/*
 * Answers the firmware's V3D clock limits and ungated rate query.
 */
int
bcm2711_firmware_get(
	uint32_t tag,
	uint32_t *values,
	unsigned request_count,
	unsigned capacity)
{
	/* Emulator identification is a mailbox-only prerequisite to native provider access. */
	if (tag == 0x00000001U) {
		assert(request_count == 0 && capacity == 1);
		values[0] = test_revision;
		return 0;
	}

	/* Raw rate readout remains meaningful before READY is granted. */
	assert(request_count == 1 && capacity == 2 && values[0] == 5);
	values[1] = test_clock_rate;
	if (tag == 0x00030007) {
		values[1] = 200000000;
	} else if (tag == 0x00030004) {
		values[1] = 500000000;
	} else {
		assert(tag == 0x00030002);
	}

	/* Succeeded: the complete provider response retains its request identity. */
	return 0;
}

/*
 * Reports zero when the provider clock is gated and its actual rate otherwise.
 */
int
bcm2711_clock_hz(
	uint32_t id,
	uint32_t *hz)
{
	/* Only the engine's validated firmware clock can be queried here. */
	assert(id == 5);
	*hz = 0;
	if (test_clock_on)
		*hz = test_clock_rate;

	/* Succeeded: register admission sees the independent provider state. */
	return 0;
}

/*
 * Records the one-microsecond bounded reset/bridge interval without wall-clock delay.
 */
void
kern_usleep_range(
	unsigned minimum,
	unsigned maximum)
{
	/* Longer or unbounded power waits violate the native provider procedure. */
	assert(minimum == 1 && maximum == 1 && test_event_count < 64);
	test_events[test_event_count++] = 201;
}

/*
 * Selects the default production stage path in this host fixture.
 */
bool
bcm2711_stage_allowed(
	const char *family,
	const char *stage)
{
	/* Physical stop parameters remain tested by the separate stage fixture. */
	(void)family;
	(void)stage;

	/* Succeeded: no boot parameter withdraws a stage. */
	return true;
}

/*
 * Keeps physical diagnostic pauses out of the host timer model.
 */
void
bcm2711_stage_pause(
	const char *family,
	const char *stage)
{
	/* The production startup still calls its normal diagnostic interface. */
	(void)family;
	(void)stage;
}

/*
 * Leaves stage messages available to the production console only.
 */
void
bcm2711_stage_mark(
	const char *family,
	const char *format,
	...)
{
	/* Host verdicts come from provider effects, never from success message text. */
	(void)family;
	(void)format;
}

/* Initializes a stopped fixture with hardware identity words and unrelated PM bits. */
static void
fixture(
	void)
{
	/* Holds the unknown engine domain inaccessible until the actual start function succeeds. */
	memset(&test_engine, 0, sizeof(test_engine));
	memset(test_pm, 0, sizeof(test_pm));
	memset(test_legacy, 0, sizeof(test_legacy));
	memset(test_bridge, 0, sizeof(test_bridge));
	test_engine.present = true;
	test_pm[0x10c / 4] = 0x1020;
	test_legacy[0x20 / 4] = 0x62726467;
	test_bridge[0x20 / 4] = 0x62726467;
	test_bridge[0x08 / 4] = 3;
	test_bridge[0x0c / 4] = 3;
	test_clock_on = false;
	test_clock_rate = 200000000;
	test_event_count = 0;
	test_stuck = 0;
	test_revision = 0x50000000;
	test_reads = 0;
}
