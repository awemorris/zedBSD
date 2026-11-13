/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the PCI power code (ws052-p004, pci-power.c).
 *
 * pci-power.c is compiled against a simulated PCI layer: five functions on
 * two buses (a bridge leads to the second), each with a configuration
 * space of 4 KiB holding the power management, MSI-X and PCI Express
 * capabilities and an LTR extended capability.  A function that leaves
 * D3hot without No_Soft_Reset loses its command register, its BARs, its
 * PCI Express and LTR registers and its MSI-X table, as hardware does.
 * Simulated drivers and platform operations write what they are asked to
 * do into a trace, which the checks compare.
 *
 *   make -C plan/ws052/tests pci-power && build/ws052/host/pci-power
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <drivers/pci/pci.h>
#include <kern/clock.h>
#include <kern/sched.h>
#include <uapi/errno.h>

#include "kern/klog.h"
#include "kern/kmem.h"

/* The size of the simulated configuration space and of the trace. */
#define CONFIG_SIZE		4096U
#define TRACE_MAX		1024U

/* Where the simulated capabilities are. */
#define CAP_PM			0x40U
#define CAP_MSIX		0x50U
#define CAP_PCIE		0x60U
#define EXT_LTR			0x100U

/* The MSI-X table: four entries of four dwords. */
#define MSIX_ENTRIES		4U

/* The simulated functions. */
#define FUNCTIONS		5U

/*
 * One simulated bus: its parent and the bridge that presents it, NULL for
 * the root.
 */
struct drv_pci_bus {
	struct drv_pci_bus *parent;
	struct drv_pci_device *bridge;
};

/*
 * One simulated function: its address, bus, driver, configuration space,
 * MSI-X table, and its name in the trace.
 */
struct drv_pci_device {
	struct drv_pci_address address;
	struct drv_pci_bus *bus;
	struct drv_pci_driver *driver;
	uint8_t config[CONFIG_SIZE];
	uint32_t msix[MSIX_ENTRIES * 4U];
	const char *name;
	bool bridge;
};

static int driver_suspend(struct drv_pci_device *device);
static int driver_resume(struct drv_pci_device *device);
static int platform_set_state(void *argument, struct drv_pci_device *device, unsigned state);
static int platform_wake(void *argument, struct drv_pci_device *device, bool wake);

/* The driver every bound function has. */
static struct drv_pci_driver test_driver = {
	.name = "test",
	.suspend = driver_suspend,
	.resume = driver_resume
};

/* A driver that cannot suspend. */
static struct drv_pci_driver plain_driver = {
	.name = "plain"
};

/* The platform operations. */
static const struct drv_pci_platform_power test_platform = {
	platform_set_state,
	platform_wake,
	NULL
};

/* The two buses and the five functions: A, bridge B, C and E on the root bus, D behind B. */
static struct drv_pci_bus root_bus;
static struct drv_pci_bus child_bus;
static struct drv_pci_device functions[FUNCTIONS];

/* What the drivers and the platform did, in order. */
static char trace[TRACE_MAX];

/* The function whose driver's suspend fails, and the one whose platform fails, or NULL. */
static struct drv_pci_device *failing_suspend;
static struct drv_pci_device *failing_platform;

/* The function whose driver's resume reports ESTALE, or NULL. */
static struct drv_pci_device *stale_resume;

/* The function whose driver keeps it in D0 across the sleep (WS237 p005), or NULL. */
static struct drv_pci_device *keep_d0;

/* How many checks failed. */
static unsigned failures;

static void reset_world(void);
static void set_function(struct drv_pci_device *device, const char *name, uint8_t bus, uint8_t slot, struct drv_pci_bus *on, struct drv_pci_driver *driver);
static void note(const char *format, ...);
static void check(bool condition, const char *what);
static void check_trace(const char *expected, const char *what);
static uint32_t config_get(const struct drv_pci_device *device, unsigned offset, unsigned bytes);
static void config_put(struct drv_pci_device *device, unsigned offset, unsigned bytes, uint32_t value);
static void power_write(struct drv_pci_device *device, uint16_t value);
static void test_order(void);
static void test_restore(void);
static void test_driver_failure(void);
static void test_platform_failure(void);
static void test_no_suspend(void);
static void test_states(void);
static void test_stale(void);
static void test_keep_d0(void);

/*
 * Runs the checks and reports how many failed.
 */
int
main(void)
{
	/* Runs each check against a fresh world. */
	test_order();
	test_restore();
	test_driver_failure();
	test_platform_failure();
	test_no_suspend();
	test_states();
	test_stale();
	test_keep_d0();

	/* Reports the outcome. */
	if (failures != 0) {
		printf("pci-power: %u checks failed\n", failures);
		return 1;
	}

	/* Succeeded: every check held. */
	printf("pci-power: every check passed\n");
	return 0;
}

/* Builds the five functions in their first state. */
static void
reset_world(void)
{
	/* The buses. */
	root_bus.parent = NULL;
	root_bus.bridge = NULL;
	child_bus.parent = &root_bus;
	child_bus.bridge = &functions[1];

	/* The functions: A a display, B a bridge without a driver, C a USB controller, E without a driver, D a disk behind B. */
	set_function(&functions[0], "A", 0, 0x02, &root_bus, &test_driver);
	set_function(&functions[1], "B", 0, 0x1c, &root_bus, NULL);
	set_function(&functions[2], "C", 0, 0x14, &root_bus, &test_driver);
	set_function(&functions[3], "E", 0, 0x1f, &root_bus, NULL);
	set_function(&functions[4], "D", 1, 0x00, &child_bus, &test_driver);
	functions[1].bridge = true;

	/* Nothing has happened yet. */
	trace[0] = '\0';
	failing_suspend = NULL;
	failing_platform = NULL;
	stale_resume = NULL;
	keep_d0 = NULL;
	drv_pci_platform_power_set(&test_platform);
}

/* Fills one function: its identity, its capabilities, a BAR, the command register and an MSI-X table. */
static void
set_function(
	struct drv_pci_device *device,
	const char *name,
	uint8_t bus,
	uint8_t slot,
	struct drv_pci_bus *on,
	struct drv_pci_driver *driver)
{
	unsigned index;

	/* The identity. */
	memset(device, 0, sizeof(*device));
	device->name = name;
	device->address.bus = bus;
	device->address.device = slot;
	device->bus = on;
	device->driver = driver;
	config_put(device, 0x00, 4, 0x12348086U);

	/* The command register, a BAR and the capability list: PM, MSI-X, PCI Express. */
	config_put(device, 0x04, 2, 0x0406U);
	config_put(device, 0x06, 2, 0x0010U);
	config_put(device, 0x10, 4, 0xfe000000U + (uint32_t)slot * 0x10000U);
	config_put(device, 0x34, 1, CAP_PM);
	config_put(device, CAP_PM, 1, 0x01);
	config_put(device, CAP_PM + 1U, 1, CAP_MSIX);
	config_put(device, CAP_MSIX, 1, 0x11);
	config_put(device, CAP_MSIX + 1U, 1, CAP_PCIE);
	config_put(device, CAP_MSIX + 2U, 2, 0x8000U | (MSIX_ENTRIES - 1U));
	config_put(device, CAP_PCIE, 1, 0x10);
	config_put(device, CAP_PCIE + 1U, 1, 0x00);
	config_put(device, CAP_PCIE + 0x10U, 2, 0x0143U);

	/* The LTR latencies. */
	config_put(device, EXT_LTR, 4, 0x00010018U);
	config_put(device, EXT_LTR + 4U, 4, 0x10031003U);

	/* The MSI-X table, which the driver programmed. */
	for (index = 0; index < MSIX_ENTRIES * 4U; index++)
		device->msix[index] = 0xfee00000U + index + (uint32_t)slot * 0x100U;
}

/* Appends one event to the trace. */
static void
note(
	const char *format,
	...)
{
	va_list arguments;
	size_t used;

	/* Appends a separator and the event. */
	used = strlen(trace);
	if (used != 0 && used < TRACE_MAX - 1U) {
		trace[used] = ' ';
		used++;
		trace[used] = '\0';
	}

	/* Writes the event after it. */
	va_start(arguments, format);
	vsnprintf(trace + used, TRACE_MAX - used, format, arguments);
	va_end(arguments);
}

/* Counts a check that failed and names it. */
static void
check(
	bool condition,
	const char *what)
{
	/* A check that held says nothing. */
	if (condition)
		return;

	/* Names the check. */
	printf("FAIL %s\n", what);
	failures++;
}

/* Compares the trace with what was expected. */
static void
check_trace(
	const char *expected,
	const char *what)
{
	int compared;

	/* Compares the two. */
	compared = strcmp(trace, expected);
	if (compared == 0)
		return;

	/* Shows both. */
	printf("FAIL %s\n  expected: %s\n  got:      %s\n", what, expected, trace);
	failures++;
}

/* Reads a little-endian value of the simulated configuration space. */
static uint32_t
config_get(
	const struct drv_pci_device *device,
	unsigned offset,
	unsigned bytes)
{
	uint32_t value;
	unsigned index;

	/* Gathers the bytes, the lowest first. */
	value = 0;
	for (index = 0; index < bytes; index++)
		value |= (uint32_t)device->config[offset + index] << (index * 8U);

	/* Reports the value. */
	return value;
}

/* Writes a little-endian value of the simulated configuration space. */
static void
config_put(
	struct drv_pci_device *device,
	unsigned offset,
	unsigned bytes,
	uint32_t value)
{
	unsigned index;

	/* Stores the bytes, the lowest first. */
	for (index = 0; index < bytes; index++)
		device->config[offset + index] = (uint8_t)(value >> (index * 8U));
}

/*
 * Plays a write to PMCSR: PME_Status clears when written as one, and a
 * function that leaves D3hot without No_Soft_Reset loses its state.
 */
static void
power_write(
	struct drv_pci_device *device,
	uint16_t value)
{
	uint16_t old;
	uint16_t stored;
	unsigned index;

	/* Keeps PME_Status unless it is written as one. */
	old = (uint16_t)config_get(device, CAP_PM + 4U, 2);
	stored = (uint16_t)(value & 0x7fffU);
	if ((value & 0x8000U) == 0)
		stored |= (uint16_t)(old & 0x8000U);

	/* Stores the register. */
	config_put(device, CAP_PM + 4U, 2, stored);

	/* Leaving D3hot resets the function. */
	if ((old & 3U) == 3U && (stored & 3U) == 0U) {
		config_put(device, 0x04, 2, 0);
		config_put(device, 0x10, 4, 0);
		config_put(device, CAP_MSIX + 2U, 2, MSIX_ENTRIES - 1U);
		config_put(device, CAP_PCIE + 0x10U, 2, 0);
		config_put(device, EXT_LTR + 4U, 4, 0);
		for (index = 0; index < MSIX_ENTRIES * 4U; index++)
			device->msix[index] = 0;
	}
}

/* Suspends and resumes everything, and checks the order of every step. */
static void
test_order(void)
{
	struct drv_pci_device *failed;
	int error;

	/* Suspends: D behind the bridge first, then A and C in bus order; C asks for its wake. */
	reset_world();
	error = drv_pci_suspend_all(&failed);
	check(error == 0, "order: the suspend succeeds");
	check(failed == NULL, "order: no function failed");
	check_trace("suspend:D platform:D:3 suspend:A platform:A:3 suspend:C wake:C:1 platform:C:3", "order: suspend");

	/* Every bound function is in D3hot; the ones without a driver are not. */
	check((config_get(&functions[0], CAP_PM + 4U, 2) & 3U) == 3U, "order: A in D3hot");
	check((config_get(&functions[1], CAP_PM + 4U, 2) & 3U) == 0U, "order: the bridge B in D0");
	check((config_get(&functions[4], CAP_PM + 4U, 2) & 3U) == 3U, "order: D in D3hot");
	check((config_get(&functions[2], CAP_PM + 4U, 2) & 0x0100U) != 0, "order: C's PME_En set");

	/* A second suspend is refused. */
	error = drv_pci_suspend_all(&failed);
	check(error == EBUSY, "order: a second suspend is EBUSY");

	/* Resumes: C first, then A, then D. */
	trace[0] = '\0';
	error = drv_pci_resume_all();
	check(error == 0, "order: the resume succeeds");
	check_trace("platform:C:0 wake:C:0 resume:C platform:A:0 resume:A platform:D:0 resume:D", "order: resume");
	check((config_get(&functions[2], CAP_PM + 4U, 2) & 0x0100U) == 0, "order: C's PME_En cleared");

	/* A resume without a suspend is refused. */
	error = drv_pci_resume_all();
	check(error == EINVAL, "order: a resume without a suspend is EINVAL");
}

/* Checks that what a reset in D0 loses comes back. */
static void
test_restore(void)
{
	struct drv_pci_device *failed;
	struct drv_pci_device *device;
	uint32_t msix[MSIX_ENTRIES * 4U];
	int error;

	/* Remembers D's state, suspends and resumes. */
	reset_world();
	device = &functions[4];
	memcpy(msix, device->msix, sizeof(msix));
	error = drv_pci_suspend_all(&failed);
	check(error == 0, "restore: the suspend succeeds");
	error = drv_pci_resume_all();
	check(error == 0, "restore: the resume succeeds");

	/* The command register, the BAR, the link control, the LTR latencies, MSI-X and its table are back. */
	check(config_get(device, 0x04, 2) == 0x0406U, "restore: the command register");
	check(config_get(device, 0x10, 4) == 0xfe000000U, "restore: the BAR");
	check(config_get(device, CAP_PCIE + 0x10U, 2) == 0x0143U, "restore: the link control");
	check(config_get(device, EXT_LTR + 4U, 4) == 0x10031003U, "restore: the LTR latencies");
	check(config_get(device, CAP_MSIX + 2U, 2) == (0x8000U | (MSIX_ENTRIES - 1U)), "restore: MSI-X enabled");
	check(memcmp(msix, device->msix, sizeof(msix)) == 0, "restore: the MSI-X table");
	check((config_get(device, CAP_PM + 4U, 2) & 3U) == 0U, "restore: D in D0");
}

/* Checks that a driver's failure resumes the functions suspended before it and names the function. */
static void
test_driver_failure(void)
{
	struct drv_pci_device *failed;
	int error;

	/* C's driver refuses: D and A are resumed, the last suspended first. */
	reset_world();
	failing_suspend = &functions[2];
	error = drv_pci_suspend_all(&failed);
	check(error == EBUSY, "driver failure: the driver's error is reported");
	check(failed == &functions[2], "driver failure: C is named");
	check_trace("suspend:D platform:D:3 suspend:A platform:A:3 suspend:C platform:A:0 resume:A platform:D:0 resume:D", "driver failure: trace");
	check((config_get(&functions[0], CAP_PM + 4U, 2) & 3U) == 0U, "driver failure: A back in D0");
	check(config_get(&functions[0], 0x10, 4) == 0xfe020000U, "driver failure: A's BAR back");

	/* The suspend can be tried again. */
	failing_suspend = NULL;
	trace[0] = '\0';
	error = drv_pci_suspend_all(&failed);
	check(error == 0, "driver failure: a new suspend succeeds");
	error = drv_pci_resume_all();
	check(error == 0, "driver failure: and resumes");
}

/* Checks that the platform's failure undoes the function's own steps too. */
static void
test_platform_failure(void)
{
	struct drv_pci_device *failed;
	int error;

	/* The platform refuses A: A is brought back and resumed, then D. */
	reset_world();
	failing_platform = &functions[0];
	error = drv_pci_suspend_all(&failed);
	check(error == EIO, "platform failure: the platform's error is reported");
	check(failed == &functions[0], "platform failure: A is named");
	check_trace("suspend:D platform:D:3 suspend:A platform:A:3 resume:A platform:D:0 resume:D", "platform failure: trace");
	check((config_get(&functions[0], CAP_PM + 4U, 2) & 3U) == 0U, "platform failure: A back in D0");
	check(config_get(&functions[0], 0x04, 2) == 0x0406U, "platform failure: A's command register back");
}

/* Checks that a bound driver without a suspend stops the suspend. */
static void
test_no_suspend(void)
{
	struct drv_pci_device *failed;
	int error;

	/* A's driver cannot suspend: D, suspended before it, is resumed. */
	reset_world();
	functions[0].driver = &plain_driver;
	error = drv_pci_suspend_all(&failed);
	check(error == ENOTSUP, "no suspend: ENOTSUP");
	check(failed == &functions[0], "no suspend: A is named");
	check_trace("suspend:D platform:D:3 platform:D:0 resume:D", "no suspend: trace");
}

/* Checks the power state calls on their own. */
static void
test_states(void)
{
	int error;

	/* D0 to D3hot and back, and a state that is refused. */
	reset_world();
	error = drv_pci_device_set_power_state(&functions[0], DRV_PCI_D3_HOT);
	check(error == 0, "states: D3hot");
	error = drv_pci_device_set_power_state(&functions[0], DRV_PCI_D3_HOT);
	check(error == 0, "states: D3hot again does nothing");
	error = drv_pci_device_set_power_state(&functions[0], 2U);
	check(error == EINVAL, "states: D2 refused");
	error = drv_pci_device_set_power_state(&functions[0], DRV_PCI_D0);
	check(error == 0, "states: D0");

	/* A wake asked for outside a suspend is refused. */
	error = drv_pci_device_set_wake(&functions[0], true);
	check(error == EINVAL, "states: a wake outside a suspend is EINVAL");
}

/* Checks that a function that lost its state in the sleep is attached again. */
static void
test_stale(void)
{
	struct drv_pci_device *failed;
	int error;

	/* C's resume reports ESTALE: it is reprobed, and the resume still succeeds. */
	reset_world();
	stale_resume = &functions[2];
	error = drv_pci_suspend_all(&failed);
	check(error == 0, "stale: the suspend succeeds");
	trace[0] = '\0';
	error = drv_pci_resume_all();
	check(error == 0, "stale: the resume succeeds after the new attach");
	check_trace("platform:C:0 wake:C:0 resume:C reprobe:C platform:A:0 resume:A platform:D:0 resume:D", "stale: trace");
}

/*
 * Checks a function its driver keeps in D0 (WS237 p005): A stays in D0
 * with its platform powered, the others sleep as before, and the resume
 * gives A only its driver's resume.
 */
static void
test_keep_d0(void)
{
	struct drv_pci_device *failed;
	int error;

	/* A is kept in D0. */
	reset_world();
	keep_d0 = &functions[0];
	error = drv_pci_suspend_all(&failed);
	check(error == 0, "keep D0: the suspend succeeds");
	check_trace("suspend:D platform:D:3 suspend:A suspend:C wake:C:1 platform:C:3", "keep D0: suspend");
	check((config_get(&functions[0], CAP_PM + 4U, 2) & 3U) == 0U, "keep D0: A in D0");
	check((config_get(&functions[4], CAP_PM + 4U, 2) & 3U) == 3U, "keep D0: D in D3hot");

	/* The resume: A gets its driver's resume only. */
	trace[0] = '\0';
	error = drv_pci_resume_all();
	check(error == 0, "keep D0: the resume succeeds");
	check_trace("platform:C:0 wake:C:0 resume:C resume:A platform:D:0 resume:D", "keep D0: resume");

	/* Outside a suspend the request is refused. */
	error = drv_pci_device_keep_d0(&functions[0], true);
	check(error == EINVAL, "keep D0: outside a suspend is EINVAL");
}

/* The simulated driver's suspend: C asks for its wake, the kept function stays in D0, and the failing function refuses. */
static int
driver_suspend(
	struct drv_pci_device *device)
{
	int error;

	/* Notes the call. */
	note("suspend:%s", device->name);

	/* The failing function refuses. */
	if (device == failing_suspend)
		return EBUSY;

	/* The function kept in D0 says so. */
	if (device == keep_d0) {
		error = drv_pci_device_keep_d0(device, true);
		if (error != 0)
			return error;
	}

	/* C, a USB controller, wakes the system. */
	if (device == &functions[2]) {
		error = drv_pci_device_set_wake(device, true);
		if (error != 0)
			return error;
	}

	/* Succeeded: the function is stopped. */
	return 0;
}

/* The simulated driver's resume; the stale function reports that it lost its state. */
static int
driver_resume(
	struct drv_pci_device *device)
{
	/* Notes the call. */
	note("resume:%s", device->name);
	if (device == stale_resume)
		return ESTALE;

	/* Succeeded: the function runs. */
	return 0;
}

/* The simulated platform's state change. */
static int
platform_set_state(
	void *argument,
	struct drv_pci_device *device,
	unsigned state)
{
	(void)argument;

	/* Notes the call; the failing function's D3hot is refused. */
	note("platform:%s:%u", device->name, state);
	if (device == failing_platform && state == DRV_PCI_D3_HOT)
		return EIO;

	/* Succeeded: the platform followed. */
	return 0;
}

/* The simulated platform's wake. */
static int
platform_wake(
	void *argument,
	struct drv_pci_device *device,
	bool wake)
{
	(void)argument;

	/* Notes the call. */
	note("wake:%s:%u", device->name, (unsigned)wake);

	/* Succeeded: the platform armed or disarmed the wake. */
	return 0;
}

/*
 * The simulated PCI layer.
 */

/* Visits the functions: the root bus in its order, then the bus behind the bridge. */
int
drv_pci_foreach_device(
	drv_pci_device_iterator_t visitor,
	void *argument)
{
	unsigned index;
	int decision;

	/* Visits each function. */
	for (index = 0; index < FUNCTIONS; index++) {
		/* Stops when the visitor asks. */
		decision = visitor(&functions[index], argument);
		if (decision != 0)
			return decision;
	}

	/* Succeeded: every function was visited. */
	return 0;
}

/* Reports a function's driver. */
struct drv_pci_driver *
drv_pci_device_driver(
	const struct drv_pci_device *device)
{
	/* Reports it. */
	return device->driver;
}

/* Reports a function's bus. */
struct drv_pci_bus *
drv_pci_device_bus(
	const struct drv_pci_device *device)
{
	/* Reports it. */
	return device->bus;
}

/* Reports the function above a function: the bridge of its bus. */
struct drv_pci_device *
drv_pci_device_upstream(
	const struct drv_pci_device *device)
{
	/* Reports it. */
	return device->bus->bridge;
}

/* Reports a function's address. */
void
drv_pci_device_address(
	const struct drv_pci_device *device,
	struct drv_pci_address *address)
{
	/* Copies it. */
	*address = device->address;
}

/* Reads a byte of the configuration space. */
int
drv_pci_device_config_read8(
	struct drv_pci_device *device,
	unsigned offset,
	uint8_t *value)
{
	/* Reads it. */
	*value = (uint8_t)config_get(device, offset, 1);
	return 0;
}

/* Reads a word of the configuration space. */
int
drv_pci_device_config_read16(
	struct drv_pci_device *device,
	unsigned offset,
	uint16_t *value)
{
	/* Reads it. */
	*value = (uint16_t)config_get(device, offset, 2);
	return 0;
}

/* Reads a dword of the configuration space. */
int
drv_pci_device_config_read32(
	struct drv_pci_device *device,
	unsigned offset,
	uint32_t *value)
{
	/* Reads it. */
	*value = config_get(device, offset, 4);
	return 0;
}

/* Writes a word of the configuration space; PMCSR has its own rules. */
int
drv_pci_device_config_write16(
	struct drv_pci_device *device,
	unsigned offset,
	uint16_t value)
{
	/* PMCSR. */
	if (offset == CAP_PM + 4U) {
		power_write(device, value);
		return 0;
	}

	/* Anything else stores. */
	config_put(device, offset, 2, value);
	return 0;
}

/* Writes a dword of the configuration space. */
int
drv_pci_device_config_write32(
	struct drv_pci_device *device,
	unsigned offset,
	uint32_t value)
{
	/* Stores it. */
	config_put(device, offset, 4, value);
	return 0;
}

/* Finds a capability in the list. */
int
drv_pci_device_find_capability(
	struct drv_pci_device *device,
	uint8_t id,
	unsigned *result)
{
	unsigned offset;
	uint32_t found;

	/* Follows the list. */
	offset = config_get(device, 0x34, 1);
	while (offset != 0) {
		/* The capability looked for. */
		found = config_get(device, offset, 1);
		if (found == id) {
			*result = offset;
			return 0;
		}

		/* The next one. */
		offset = config_get(device, offset + 1U, 1);
	}

	/* The function lacks it. */
	return ENOENT;
}

/* Finds an extended capability: only LTR exists. */
int
drv_pci_device_find_extended_capability(
	struct drv_pci_device *device,
	uint16_t id,
	unsigned start,
	unsigned *result)
{
	(void)device;
	(void)start;

	/* LTR is at its place. */
	if (id == 0x0018U) {
		*result = EXT_LTR;
		return 0;
	}

	/* The rest is absent. */
	return ENOENT;
}

/* Maps the MSI-X table: the simulated table itself. */
int
drv_pci_device_map_msix_table(
	struct drv_pci_device *device,
	struct drv_pci_mapping *mapping,
	unsigned *entries)
{
	/* Hands the table over. */
	memset(mapping, 0, sizeof(*mapping));
	mapping->address = device->msix;
	mapping->size = sizeof(device->msix);
	*entries = MSIX_ENTRIES;
	return 0;
}

/* Detaches and attaches a function again: noted in the trace. */
int
drv_pci_device_reprobe(
	struct drv_pci_device *device)
{
	/* Notes it. */
	note("reprobe:%s", device->name);
	return 0;
}

/* Unmaps a mapping: nothing to do. */
void
drv_pci_device_unmap_bar(
	struct drv_pci_device *device,
	struct drv_pci_mapping *mapping)
{
	(void)device;
	(void)mapping;
}

/*
 * The simulated kernel.
 */

/* Allocates from the host heap. */
void *
kern_malloc(
	size_t size)
{
	/* Allocates. */
	return malloc(size);
}

/* Frees to the host heap. */
void
kern_free(
	void *pointer)
{
	/* Frees. */
	free(pointer);
}

/* Drops the kernel's log lines, which the trace does not compare. */
void
kern_logf(
	const char *format,
	...)
{
	(void)format;
}

/* The tick counter: always zero. */
uint64_t
sched_ticks(void)
{
	/* Reports it. */
	return 0;
}

/* Milliseconds as ticks: one each, which the sleep ignores. */
uint64_t
kern_ms_to_ticks(
	uint64_t milliseconds)
{
	/* Reports it. */
	return milliseconds;
}

/* A sleep returns at once. */
void
sched_sleep(
	uint64_t timeout_tick)
{
	(void)timeout_tick;
}
