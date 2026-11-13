/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of S0 idle behind a VMD: pci-power.c and the VMD driver
 * (pci-vmd.c, compiled into this file so that its host can be built
 * without an attach; pci-vmd-plan.c and pci-window.c as they are) against
 * a simulated machine.
 *
 * The machine: a VMD V on the platform's root bus, whose CFGBAR (a buffer
 * here, read and written through the driver's own accessors) holds a root
 * port R (bus e0, no driver) and an NVMe disk N behind it (bus e1, a test
 * driver); a USB controller C and a root port P without a driver on the
 * platform's root bus.  What the hardware does is played by the accessors:
 * nothing behind V answers while V is in D3hot, has memory decoding off or
 * decodes another bus range; N does not answer while R lacks its bus
 * numbers or its link is down.  V leaving D3hot without No_Soft_Reset
 * resets V (its header, configuration register and MSI-X table), R (bus
 * numbers, windows, PCI Express controls, link down for a few reads of its
 * link status) and N.  V's PMCSR says No_Soft_Reset, so that its driver
 * lets it go to D3hot, and the machine resets it all the same, so that
 * every write-back is exercised; a V without the bit stays in D0.
 *
 *   sh plan/bugs/BUG-424/run.sh build/bug424
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "drivers/pci/pci-vmd.c"

#include <kern/clock.h>
#include <kern/sched.h>

/* The size of one simulated configuration space, and of the trace and the log. */
#define CONFIG_SIZE		4096U
#define TRACE_MAX		8192U
#define LOG_MAX			4096U

/* The CFGBAR: buses e0 and e1. */
#define BUS_START		0xe0U
#define BUS_END			0xe1U
#define CFGBAR_SIZE		(UINT64_C(2) << 20)

/* V's registers: its BARs, its own two registers, its PM and MSI-X capabilities. */
#define V_CAPABILITY_VALUE	0x0001U
#define V_CONFIGURATION_VALUE	0x0200U
#define V_PM			0x80U
#define V_MSIX			0x90U
#define V_CFGBAR		UINT64_C(0x6000000000)
#define V_MEMBAR1		UINT64_C(0x6100000000)
#define V_MEMBAR2		UINT64_C(0x6200000000)

/* R's capabilities: PM and PCI Express (a root port with a slot that reports its link). */
#define R_PM			0x40U
#define R_PCIE			0x60U

/* N's and C's capabilities: PM, MSI-X, PCI Express and LTR. */
#define E_PM			0x40U
#define E_MSIX			0x50U
#define E_PCIE			0x60U
#define E_LTR			0x100U

/* Every MSI-X table has four entries. */
#define MSIX_ENTRIES		4U

/* How many reads of R's link status the link stays down for after a reset. */
#define LINK_DOWN_READS		3U

/*
 * One simulated bus: the bridge that presents it, and the function that
 * presents a root bus.
 */
struct drv_pci_bus {
	struct drv_pci_device *bridge;
	struct drv_pci_device *host_function;
};

/*
 * One simulated function: its address, bus, driver and driver data, its
 * configuration space (its own, or a place in V's CFGBAR), its MSI-X
 * table, its name in the trace, and how many times it was touched.
 */
struct drv_pci_device {
	struct drv_pci_address address;
	struct drv_pci_bus *bus;
	struct drv_pci_driver *driver;
	void *driver_data;
	uint8_t own[CONFIG_SIZE];
	uint8_t *config;
	uint32_t msix[MSIX_ENTRIES * 4U];
	const char *name;
	unsigned pm;
	bool behind_vmd;
	unsigned touched;
};

static int driver_suspend(struct drv_pci_device *device);
static int driver_resume(struct drv_pci_device *device);
static int platform_set_state(void *argument, struct drv_pci_device *device, unsigned state);

/* The driver of N and C. */
static struct drv_pci_driver test_driver = {
	.name = "test",
	.suspend = driver_suspend,
	.resume = driver_resume
};

/* The platform's operations: they only write the trace. */
static const struct drv_pci_platform_power test_platform = {
	platform_set_state,
	NULL,
	NULL
};

/* The buses: the platform's root bus, the VMD's root bus, and the bus behind R. */
static struct drv_pci_bus platform_bus;
static struct drv_pci_bus vmd_bus;
static struct drv_pci_bus port_bus;

/* The functions, in the order the walk visits them: V's bus first, as the core keeps it. */
static struct drv_pci_device v_function;
static struct drv_pci_device r_function;
static struct drv_pci_device n_function;
static struct drv_pci_device c_function;
static struct drv_pci_device p_function;
static struct drv_pci_device *const walk_order[] = {
	&r_function, &n_function, &v_function, &c_function, &p_function
};

/* The VMD's host and its CFGBAR. */
static struct vmd_host vmd;
static uint8_t *cfgbar;

/* What happened, in order, and what the kernel logged. */
static char trace[TRACE_MAX];
static char klog[LOG_MAX];

/* Played faults: C's suspend fails, V's bus range cannot be written back, R is gone after V's reset. */
static bool c_refuses;
static bool v_range_locked;
static bool r_lost;

/* V's No_Soft_Reset, and R's link: down, and for how many more reads of its status. */
static bool v_no_soft_reset;
static unsigned link_reads_left;

/* How long the code slept, in milliseconds, and how many checks failed. */
static uint64_t slept_ms;
static unsigned failures;

static void reset_world(void);
static void fill_vmd(void);
static void fill_port(void);
static void fill_endpoint(struct drv_pci_device *device, uint32_t bar);
static void note(const char *format, ...);
static void check(bool condition, const char *what);
static long position(const char *event);
static void check_before(const char *first, const char *second, const char *what);
static uint32_t get(const uint8_t *config, unsigned offset, unsigned bytes);
static void put(uint8_t *config, unsigned offset, unsigned bytes, uint32_t value);
static bool reachable(const struct drv_pci_device *device);
static uint32_t sim_read(struct drv_pci_device *device, unsigned offset, unsigned bytes);
static void sim_write(struct drv_pci_device *device, unsigned offset, unsigned bytes, uint32_t value);
static void power_written(struct drv_pci_device *device, uint16_t old);
static void reset_vmd(void);
static void reset_port(void);
static void reset_endpoint(struct drv_pci_device *device);
static void test_round_trip(void);
static void test_state_kept(void);
static void test_range_locked(void);
static void test_later_failure(void);
static void test_depth(void);
static void unexpected(const char *name);
static void test_vmd_in_d0(void);
static void test_port_lost(void);

/*
 * Runs the checks and reports how many failed.
 */
int
main(void)
{
	/* The CFGBAR. */
	cfgbar = calloc(1, (size_t)CFGBAR_SIZE);
	if (cfgbar == NULL)
		return 2;

	/* Runs each check against a fresh machine. */
	test_depth();
	test_round_trip();
	test_state_kept();
	test_range_locked();
	test_later_failure();
	test_vmd_in_d0();
	test_port_lost();
	free(cfgbar);

	/* Reports the outcome. */
	if (failures != 0) {
		printf("vmd-power: %u checks failed\n", failures);
		return 1;
	}

	/* Succeeded: every check held. */
	printf("vmd-power: every check passed\n");
	return 0;
}

/* Builds the machine in the state the attach left it in. */
static void
reset_world(void)
{
	uint64_t place;

	/* The buses: V's root bus is presented by V, the port's bus by R. */
	memset(&platform_bus, 0, sizeof(platform_bus));
	memset(&vmd_bus, 0, sizeof(vmd_bus));
	memset(&port_bus, 0, sizeof(port_bus));
	vmd_bus.host_function = &v_function;
	port_bus.bridge = &r_function;

	/* The functions. */
	memset(&v_function, 0, sizeof(v_function));
	memset(&r_function, 0, sizeof(r_function));
	memset(&n_function, 0, sizeof(n_function));
	memset(&c_function, 0, sizeof(c_function));
	memset(&p_function, 0, sizeof(p_function));
	memset(cfgbar, 0, (size_t)CFGBAR_SIZE);

	/* V on the platform's bus, with its own configuration space. */
	v_function.name = "V";
	v_function.address.device = 0x0e;
	v_function.bus = &platform_bus;
	v_function.driver = &vmd_driver;
	v_function.driver_data = &vmd;
	v_function.config = v_function.own;
	v_function.pm = V_PM;

	/* R and N in the CFGBAR. */
	r_function.name = "R";
	r_function.address.segment = 0xffffU;
	r_function.address.bus = BUS_START;
	r_function.address.device = 6;
	r_function.bus = &vmd_bus;
	r_function.pm = R_PM;
	r_function.behind_vmd = true;
	(void)drv_pci_vmd_config_offset(BUS_START, BUS_END, CFGBAR_SIZE, BUS_START, 6, 0, 0, 4, &place);
	r_function.config = cfgbar + place;
	n_function.name = "N";
	n_function.address.segment = 0xffffU;
	n_function.address.bus = BUS_END;
	n_function.bus = &port_bus;
	n_function.driver = &test_driver;
	n_function.pm = E_PM;
	n_function.behind_vmd = true;
	(void)drv_pci_vmd_config_offset(BUS_START, BUS_END, CFGBAR_SIZE, BUS_END, 0, 0, 0, 4, &place);
	n_function.config = cfgbar + place;

	/* C and P on the platform's bus. */
	c_function.name = "C";
	c_function.address.device = 0x14;
	c_function.bus = &platform_bus;
	c_function.driver = &test_driver;
	c_function.config = c_function.own;
	c_function.pm = E_PM;
	p_function.name = "P";
	p_function.address.device = 0x1c;
	p_function.bus = &platform_bus;
	p_function.config = p_function.own;

	/* Their registers. */
	fill_vmd();
	fill_port();
	fill_endpoint(&n_function, 0x80000004U);
	fill_endpoint(&c_function, 0x90000004U);
	put(p_function.config, 0x00, 4, 0x51b08086U);
	put(p_function.config, 0x0c, 4, 0x00010000U);

	/* V's host as the attach leaves it. */
	memset(&vmd, 0, sizeof(vmd));
	vmd.device = &v_function;
	vmd.segment = 0xffffU;
	vmd.cfgbar.address = cfgbar;
	vmd.cfgbar.physical_address = V_CFGBAR;
	vmd.cfgbar_size = CFGBAR_SIZE;
	vmd.membar1.bus_address = V_MEMBAR1;
	vmd.membar1.size = UINT64_C(0x2000000);
	vmd.membar2.bus_address = V_MEMBAR2;
	vmd.membar2.size = UINT64_C(0x100000);
	vmd.domain.bus_start = BUS_START;
	vmd.domain.bus_end = BUS_END;
	vmd.root = &vmd_bus;

	/* Nothing has happened yet. */
	trace[0] = '\0';
	klog[0] = '\0';
	c_refuses = false;
	v_range_locked = false;
	r_lost = false;
	v_no_soft_reset = false;
	link_reads_left = 0;
	slept_ms = 0;
	drv_pci_platform_power_set(&test_platform);
}

/* Fills V: its identity, BARs, its two registers, PM and an MSI-X table its driver programmed. */
static void
fill_vmd(void)
{
	uint8_t *config;
	unsigned index;

	/* The identity, the command register and the capability list. */
	config = v_function.config;
	put(config, 0x00, 4, 0x467f8086U);
	put(config, 0x04, 2, 0x0006U);
	put(config, 0x06, 2, 0x0010U);
	put(config, 0x08, 4, 0x01040000U);

	/* The three 64-bit BARs. */
	put(config, 0x10, 4, (uint32_t)V_CFGBAR | 0x0cU);
	put(config, 0x14, 4, (uint32_t)(V_CFGBAR >> 32));
	put(config, 0x18, 4, (uint32_t)V_MEMBAR1 | 0x0cU);
	put(config, 0x1c, 4, (uint32_t)(V_MEMBAR1 >> 32));
	put(config, 0x20, 4, (uint32_t)V_MEMBAR2 | 0x04U);
	put(config, 0x24, 4, (uint32_t)(V_MEMBAR2 >> 32));

	/* The capability and configuration registers: bus range e0, the messages caught. */
	put(config, 0x40, 2, V_CAPABILITY_VALUE);
	put(config, 0x44, 2, V_CONFIGURATION_VALUE);

	/* PM, then MSI-X with its table enabled. */
	put(config, 0x34, 1, V_PM);
	put(config, V_PM, 1, 0x01);
	put(config, V_PM + 1U, 1, V_MSIX);
	put(config, V_PM + 4U, 2, 0x0008U);
	put(config, V_MSIX, 1, 0x11);
	put(config, V_MSIX + 2U, 2, 0x8000U | (MSIX_ENTRIES - 1U));

	/* Entry 1 is the children's vector, unmasked; the others masked. */
	for (index = 0; index < MSIX_ENTRIES * 4U; index++)
		v_function.msix[index] = 0xfee00000U + index;
	v_function.msix[0 * 4U + 3U] = 1U;
	v_function.msix[1 * 4U + 3U] = 0U;
	v_function.msix[2 * 4U + 3U] = 1U;
	v_function.msix[3 * 4U + 3U] = 1U;
}

/* Fills R: a root port with bus numbers, a window and its PCI Express controls, link up. */
static void
fill_port(void)
{
	uint8_t *config;

	/* The identity, the command register, the bridge's header. */
	config = r_function.config;
	put(config, 0x00, 4, 0x464d8086U);
	put(config, 0x04, 2, 0x0407U);
	put(config, 0x06, 2, 0x0010U);
	put(config, 0x08, 4, 0x06040001U);
	put(config, 0x0c, 4, 0x00010010U);
	put(config, 0x18, 4, 0x00e1e1e0U);
	put(config, 0x1c, 4, 0x000000f0U);
	put(config, 0x20, 4, 0x6100610fU);
	put(config, 0x24, 4, 0x0001fff1U);
	put(config, 0x3c, 4, 0x000201ffU);

	/* PM, in D0 throughout. */
	put(config, 0x34, 1, R_PM);
	put(config, R_PM, 1, 0x01);
	put(config, R_PM + 1U, 1, R_PCIE);

	/* PCI Express: a root port with a slot, reporting its link, which is up. */
	put(config, R_PCIE, 1, 0x10);
	put(config, R_PCIE + 0x02U, 2, 0x0142U);
	put(config, R_PCIE + 0x08U, 2, 0x2817U);
	put(config, R_PCIE + 0x0cU, 4, 0x00327c43U);
	put(config, R_PCIE + 0x10U, 2, 0x0142U);
	put(config, R_PCIE + 0x12U, 2, 0x3043U);
	put(config, R_PCIE + 0x18U, 2, 0x1008U);
	put(config, R_PCIE + 0x1cU, 2, 0x0008U);
	put(config, R_PCIE + 0x28U, 2, 0x0400U);
	put(config, R_PCIE + 0x30U, 2, 0x0004U);
}

/* Fills an endpoint: a 64-bit BAR, PM, MSI-X, PCI Express and LTR. */
static void
fill_endpoint(
	struct drv_pci_device *device,
	uint32_t bar)
{
	uint8_t *config;
	unsigned index;

	/* The identity, the command register and the BAR. */
	config = device->config;
	put(config, 0x00, 4, 0x5845144dU);
	put(config, 0x04, 2, 0x0406U);
	put(config, 0x06, 2, 0x0010U);
	put(config, 0x10, 4, bar);
	put(config, 0x14, 4, 0x00000061U);

	/* PM, MSI-X and PCI Express (an endpoint). */
	put(config, 0x34, 1, E_PM);
	put(config, E_PM, 1, 0x01);
	put(config, E_PM + 1U, 1, E_MSIX);
	put(config, E_MSIX, 1, 0x11);
	put(config, E_MSIX + 1U, 1, E_PCIE);
	put(config, E_MSIX + 2U, 2, 0x8000U | (MSIX_ENTRIES - 1U));
	put(config, E_PCIE, 1, 0x10);
	put(config, E_PCIE + 0x02U, 2, 0x0002U);
	put(config, E_PCIE + 0x10U, 2, 0x0142U);

	/* LTR. */
	put(config, E_LTR, 4, 0x00010018U);
	put(config, E_LTR + 4U, 4, 0x10031003U);

	/* The MSI-X table its driver programmed. */
	for (index = 0; index < MSIX_ENTRIES * 4U; index++)
		device->msix[index] = 0xfee01000U + index;
}

/* Appends one event to the trace. */
static void
note(
	const char *format,
	...)
{
	va_list arguments;
	size_t used;

	/* Appends a separator. */
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

/* Reports where an event is first in the trace, or -1. */
static long
position(
	const char *event)
{
	char *found;

	/* Looks for the event. */
	found = strstr(trace, event);
	if (found == NULL)
		return -1;

	/* Reports its place. */
	return (long)(found - trace);
}

/* Checks that both events happened, the first one first. */
static void
check_before(
	const char *first,
	const char *second,
	const char *what)
{
	long first_place;
	long second_place;

	/* Finds both. */
	first_place = position(first);
	second_place = position(second);

	/* Shows the trace when they are missing or the other way round. */
	if (first_place < 0 || second_place < 0 || first_place >= second_place) {
		printf("FAIL %s (%s at %ld, %s at %ld)\n  trace: %s\n", what, first, first_place, second, second_place, trace);
		failures++;
	}
}

/* Reads a little-endian value of a configuration space. */
static uint32_t
get(
	const uint8_t *config,
	unsigned offset,
	unsigned bytes)
{
	uint32_t value;
	unsigned index;

	/* Gathers the bytes, the lowest first. */
	value = 0;
	for (index = 0; index < bytes; index++)
		value |= (uint32_t)config[offset + index] << (index * 8U);

	/* Reports the value. */
	return value;
}

/* Writes a little-endian value of a configuration space. */
static void
put(
	uint8_t *config,
	unsigned offset,
	unsigned bytes,
	uint32_t value)
{
	unsigned index;

	/* Stores the bytes, the lowest first. */
	for (index = 0; index < bytes; index++)
		config[offset + index] = (uint8_t)(value >> (index * 8U));
}

/*
 * Reports whether a function behind V answers: V in D0, decoding memory
 * and the bus range e0; for N also R's bus numbers and its link.
 */
static bool
reachable(
	const struct drv_pci_device *device)
{
	uint32_t configuration;
	uint32_t buses;

	/* A function on the platform's bus always answers. */
	if (!device->behind_vmd)
		return true;

	/* V must be in D0 and decode memory. */
	if ((get(v_function.config, V_PM + 4U, 2) & 3U) != 0)
		return false;
	if ((get(v_function.config, 0x04, 2) & 0x0002U) == 0)
		return false;

	/* V must decode the bus range starting at e0. */
	configuration = get(v_function.config, 0x44, 2);
	if (((configuration >> 8) & 3U) != 2U)
		return false;

	/* R answers once V does, unless V's reset lost it. */
	if (device == &r_function)
		return !r_lost;

	/* N needs R's bus numbers and an active link. */
	buses = get(r_function.config, 0x18, 4);
	if (((buses >> 8) & 0xffU) != BUS_END || ((buses >> 16) & 0xffU) < BUS_END)
		return false;
	if ((get(r_function.config, R_PCIE + 0x12U, 2) & 0x2000U) == 0)
		return false;

	/* N answers. */
	return true;
}

/* Reads a register as the hardware answers; behind V through the driver's CFGBAR accessor. */
static uint32_t
sim_read(
	struct drv_pci_device *device,
	unsigned offset,
	unsigned bytes)
{
	uint32_t value;
	uint16_t status;

	/* Counts the access, and notes the reads the checks look for. */
	device->touched++;
	if (device == &v_function && offset == 0x44)
		note("r:V:44");
	if (device == &r_function && offset == R_PCIE + 0x12U)
		note("r:R:72");

	/* A function that does not answer reads as all ones. */
	if (!reachable(device)) {
		if (bytes == 4U)
			return 0xffffffffU;
		return (1U << (bytes * 8U)) - 1U;
	}

	/* A link that is down trains while its status is read. */
	if (device == &r_function && offset == R_PCIE + 0x12U && link_reads_left != 0) {
		link_reads_left--;
		if (link_reads_left == 0) {
			status = (uint16_t)get(r_function.config, R_PCIE + 0x12U, 2);
			put(r_function.config, R_PCIE + 0x12U, 2, status | 0x2000U);
		}
	}

	/* A function on the platform's bus is read directly. */
	if (!device->behind_vmd)
		return get(device->config, offset, bytes);

	/* One behind V is read through the driver's CFGBAR accessor. */
	value = 0;
	(void)vmd_bus_ops.config_read(&vmd, &device->address, offset, bytes, &value);
	return value;
}

/* Writes a register as the hardware takes it, and plays what the write does. */
static void
sim_write(
	struct drv_pci_device *device,
	unsigned offset,
	unsigned bytes,
	uint32_t value)
{
	uint16_t old_power;

	/* Notes and counts the write. */
	note("w:%s:%02x", device->name, offset);
	device->touched++;

	/* A function that does not answer drops it. */
	if (!reachable(device))
		return;

	/* Keeps the power state the write may change. */
	old_power = 0;
	if (device->pm != 0)
		old_power = (uint16_t)get(device->config, device->pm + 4U, 2);

	/* V's bus range is fixed while locked. */
	if (device == &v_function && offset == 0x44 && v_range_locked)
		value = (value & ~0x0300U) | (get(device->config, 0x44, 2) & 0x0300U);

	/* Writes it: directly, or through the driver's CFGBAR accessor. */
	if (!device->behind_vmd)
		put(device->config, offset, bytes, value);
	else
		(void)vmd_bus_ops.config_write(&vmd, &device->address, offset, bytes, value);

	/* Plays a write to PMCSR. */
	if (device->pm != 0 && offset == device->pm + 4U)
		power_written(device, old_power);
}

/* Plays a function leaving D3hot: without No_Soft_Reset it is reset, V with what is behind it. */
static void
power_written(
	struct drv_pci_device *device,
	uint16_t old)
{
	uint16_t now;

	/* Only D3hot to D0 resets. */
	now = (uint16_t)get(device->config, device->pm + 4U, 2);
	if ((old & 3U) != 3U || (now & 3U) != 0U)
		return;

	/* V resets with the hierarchy behind it, unless it keeps its state. */
	if (device == &v_function) {
		if (v_no_soft_reset)
			return;
		reset_vmd();
		reset_port();
		reset_endpoint(&n_function);

		/* A lost R reads as all ones in the CFGBAR too. */
		if (r_lost)
			memset(r_function.config, 0xff, CONFIG_SIZE);
		return;
	}

	/* An endpoint resets. */
	reset_endpoint(device);
}

/* Resets V: its header, its configuration register and its MSI-X table. */
static void
reset_vmd(void)
{
	unsigned index;

	/* The command register, the BARs, the configuration register and MSI-X. */
	put(v_function.config, 0x04, 2, 0);
	for (index = 0x10; index < 0x28; index += 4U)
		put(v_function.config, index, 4, 0);
	put(v_function.config, 0x44, 2, 0);
	put(v_function.config, V_MSIX + 2U, 2, MSIX_ENTRIES - 1U);
	for (index = 0; index < MSIX_ENTRIES * 4U; index++)
		v_function.msix[index] = 0;
}

/* Resets R: its command, bus numbers, windows and PCI Express controls; the link goes down. */
static void
reset_port(void)
{
	uint8_t *config;
	uint16_t status;

	/* The header. */
	config = r_function.config;
	put(config, 0x04, 2, 0);
	put(config, 0x18, 4, 0);
	put(config, 0x1c, 4, 0);
	put(config, 0x20, 4, 0);
	put(config, 0x24, 4, 0);
	put(config, 0x3c, 4, 0x000001ffU);

	/* The PCI Express controls. */
	put(config, R_PCIE + 0x08U, 2, 0);
	put(config, R_PCIE + 0x10U, 2, 0);
	put(config, R_PCIE + 0x18U, 2, 0);
	put(config, R_PCIE + 0x1cU, 2, 0);
	put(config, R_PCIE + 0x28U, 2, 0);
	put(config, R_PCIE + 0x30U, 2, 0);

	/* The link is down for a while. */
	status = (uint16_t)get(config, R_PCIE + 0x12U, 2);
	put(config, R_PCIE + 0x12U, 2, status & (uint16_t)~0x2000U);
	link_reads_left = LINK_DOWN_READS;
}

/* Resets an endpoint: D0, its command, BAR, MSI-X, link control and LTR. */
static void
reset_endpoint(
	struct drv_pci_device *device)
{
	unsigned index;

	/* The registers. */
	put(device->config, E_PM + 4U, 2, 0);
	put(device->config, 0x04, 2, 0);
	put(device->config, 0x10, 4, 0);
	put(device->config, 0x14, 4, 0);
	put(device->config, E_MSIX + 2U, 2, MSIX_ENTRIES - 1U);
	put(device->config, E_PCIE + 0x10U, 2, 0);
	put(device->config, E_LTR + 4U, 4, 0);
	for (index = 0; index < MSIX_ENTRIES * 4U; index++)
		device->msix[index] = 0;
}

/* Checks the order of the walk: N, then R, then V. */
static void
test_depth(void)
{
	struct drv_pci_device *failed;
	int error;

	/* Suspends: N's driver first, V's suspend (its register read) after R's save, C last at V's depth. */
	reset_world();
	error = drv_pci_suspend_all(&failed);
	check(error == 0, "depth: the suspend succeeds");
	check_before("suspend:N", "r:R:72", "depth: N is suspended before R is saved");
	check_before("r:R:72", "r:V:44", "depth: R is saved before V's suspend");
	check_before("r:V:44", "w:V:84", "depth: V's register is kept before V goes to D3hot");
	check_before("w:V:84", "suspend:C", "depth: C at V's depth after V, in the walk's order");

	/* P, without a driver and with nothing above it, is left alone. */
	check(p_function.touched == 0, "depth: P is not touched");
	check(vmd.saved_configuration == V_CONFIGURATION_VALUE, "depth: V's register kept");

	/* Resumes. */
	error = drv_pci_resume_all();
	check(error == 0, "depth: the resume succeeds");
}

/* Suspends and resumes with V reset in D3hot, and checks every register came back in order. */
static void
test_round_trip(void)
{
	struct drv_pci_device *failed;
	uint8_t v_before[CONFIG_SIZE];
	uint8_t r_before[CONFIG_SIZE];
	uint8_t n_before[CONFIG_SIZE];
	uint32_t v_msix[MSIX_ENTRIES * 4U];
	uint32_t n_msix[MSIX_ENTRIES * 4U];
	int error;

	/* Remembers the machine. */
	reset_world();
	memcpy(v_before, v_function.config, CONFIG_SIZE);
	memcpy(r_before, r_function.config, CONFIG_SIZE);
	memcpy(n_before, n_function.config, CONFIG_SIZE);
	memcpy(v_msix, v_function.msix, sizeof(v_msix));
	memcpy(n_msix, n_function.msix, sizeof(n_msix));

	/* Suspends: V and N in D3hot, R in D0. */
	error = drv_pci_suspend_all(&failed);
	check(error == 0, "round trip: the suspend succeeds");
	check((get(v_function.config, V_PM + 4U, 2) & 3U) == 3U, "round trip: V in D3hot");
	check((get(n_function.config, E_PM + 4U, 2) & 3U) == 3U, "round trip: N in D3hot");
	check((get(r_function.config, R_PM + 4U, 2) & 3U) == 0U, "round trip: R in D0");

	/* Resumes. */
	trace[0] = '\0';
	slept_ms = 0;
	error = drv_pci_resume_all();
	check(error == 0, "round trip: the resume succeeds");

	/* V's header first, then its register, then R's bus numbers, R's link, then N, then N's driver. */
	check_before("w:V:84", "w:V:04", "round trip: V in D0 before its header");
	check_before("w:V:04", "w:V:44", "round trip: V's header before its register");
	check_before("w:V:44", "w:R:18", "round trip: V's register before R's bus numbers");
	check_before("w:R:18", "w:R:04", "round trip: R's bus numbers before its command");
	check_before("w:R:04", "r:R:72", "round trip: R's header before its link is waited for");
	check_before("r:R:72", "w:N:3c", "round trip: R's link before N is spoken to");
	check_before("w:N:3c", "resume:N", "round trip: N's header before its driver");
	check(link_reads_left == 0, "round trip: the link came up");
	check(position("w:R:78") >= 0, "round trip: R's slot control written back");
	/* V and C get 10 ms each after D3hot; the link 10 ms a poll until it is up, then the device 100 ms. */
	check(slept_ms == 2U * 10U + 10U * (LINK_DOWN_READS - 1U) + 100U, "round trip: the link and the device were given their time");

	/* Every register is back: V's header and register, R whole, N whole, the MSI-X tables. */
	check(memcmp(v_before, v_function.config, 0x48) == 0, "round trip: V's header and registers");
	check(get(v_function.config, V_MSIX + 2U, 2) == get(v_before, V_MSIX + 2U, 2), "round trip: V's MSI-X control");
	check(memcmp(v_msix, v_function.msix, sizeof(v_msix)) == 0, "round trip: V's MSI-X table");
	check(memcmp(r_before, r_function.config, 0x100) == 0, "round trip: R's header and PCI Express controls");
	check(memcmp(n_before, n_function.config, 0x48) == 0, "round trip: N's header");
	check(get(n_function.config, E_LTR + 4U, 4) == 0x10031003U, "round trip: N's LTR");
	check(get(n_function.config, E_PCIE + 0x10U, 2) == 0x0142U, "round trip: N's link control");
	check(memcmp(n_msix, n_function.msix, sizeof(n_msix)) == 0, "round trip: N's MSI-X table");
	check(strstr(klog, "vmd: ffff: resumed") != NULL, "round trip: the VMD logs its resume");
}

/* Suspends and resumes a V that keeps its state: nothing is waited for. */
static void
test_state_kept(void)
{
	struct drv_pci_device *failed;
	uint8_t r_before[CONFIG_SIZE];
	int error;

	/* Suspends and resumes with No_Soft_Reset. */
	reset_world();
	v_no_soft_reset = true;
	memcpy(r_before, r_function.config, CONFIG_SIZE);
	error = drv_pci_suspend_all(&failed);
	check(error == 0, "state kept: the suspend succeeds");
	trace[0] = '\0';
	error = drv_pci_resume_all();
	check(error == 0, "state kept: the resume succeeds");

	/* V's register is left, and R's link is not waited for. */
	check(position("w:V:44") < 0, "state kept: V's register not written");
	check(position("r:R:72") < 0, "state kept: R's link not waited for");
	check(position("w:R:78") < 0, "state kept: R's slot control, unchanged, not written");
	check(memcmp(r_before, r_function.config, 0x100) == 0, "state kept: R unchanged");
	check_before("w:R:18", "resume:N", "state kept: R written back before N's driver");
}

/* Resumes a V whose bus range does not come back: the resume fails and says so. */
static void
test_range_locked(void)
{
	struct drv_pci_device *failed;
	int error;

	/* Suspends, and makes the bus range fixed at its reset value. */
	reset_world();
	error = drv_pci_suspend_all(&failed);
	check(error == 0, "range locked: the suspend succeeds");
	v_range_locked = true;

	/* The resume reports the VMD's failure. */
	error = drv_pci_resume_all();
	check(error == EIO, "range locked: EIO");
	check(strstr(klog, "the buses did not come back") != NULL, "range locked: logged");
}

/* A failure after V undoes V, R and N in that order, with their registers. */
static void
test_later_failure(void)
{
	struct drv_pci_device *failed;
	uint8_t r_before[CONFIG_SIZE];
	uint8_t n_before[CONFIG_SIZE];
	int error;

	/* C refuses after V went to D3hot. */
	reset_world();
	c_refuses = true;
	memcpy(r_before, r_function.config, CONFIG_SIZE);
	memcpy(n_before, n_function.config, CONFIG_SIZE);
	error = drv_pci_suspend_all(&failed);
	check(error == EBUSY, "later failure: C's error");
	check(failed == &c_function, "later failure: C is named");

	/* V came back before R, R before N. */
	check_before("suspend:C", "w:V:44", "later failure: V's register after C refused");
	check_before("w:V:44", "w:R:18", "later failure: V before R");
	check_before("w:R:18", "resume:N", "later failure: R before N");
	check(memcmp(r_before, r_function.config, 0x100) == 0, "later failure: R back");
	check(memcmp(n_before, n_function.config, 0x48) == 0, "later failure: N back");
	check((get(v_function.config, V_PM + 4U, 2) & 3U) == 0U, "later failure: V in D0");
}

/* A V without No_Soft_Reset stays in D0, and nothing behind it is lost. */
static void
test_vmd_in_d0(void)
{
	struct drv_pci_device *failed;
	uint8_t v_before[CONFIG_SIZE];
	uint8_t r_before[CONFIG_SIZE];
	int error;

	/* Clears No_Soft_Reset and suspends. */
	reset_world();
	put(v_function.config, V_PM + 4U, 2, 0x0000U);
	memcpy(v_before, v_function.config, CONFIG_SIZE);
	memcpy(r_before, r_function.config, CONFIG_SIZE);
	error = drv_pci_suspend_all(&failed);
	check(error == 0, "in D0: the suspend succeeds");
	check((get(v_function.config, V_PM + 4U, 2) & 3U) == 0U, "in D0: V stays in D0");
	check(position("w:V:84") < 0, "in D0: V's PMCSR not written");
	check(strstr(klog, "sleeps in D0") != NULL, "in D0: logged");

	/* Resumes: nothing was lost, nothing waited for. */
	trace[0] = '\0';
	error = drv_pci_resume_all();
	check(error == 0, "in D0: the resume succeeds");
	check(position("w:V:44") < 0, "in D0: V's register not written");
	check(position("r:R:72") < 0, "in D0: R's link not waited for");
	check(memcmp(v_before, v_function.config, 0x48) == 0, "in D0: V's header");
	check(memcmp(r_before, r_function.config, 0x100) == 0, "in D0: R");
}

/* A root port that does not answer after V's reset fails V's resume. */
static void
test_port_lost(void)
{
	struct drv_pci_device *failed;
	int error;

	/* Suspends, then loses R. */
	reset_world();
	error = drv_pci_suspend_all(&failed);
	check(error == 0, "port lost: the suspend succeeds");
	r_lost = true;

	/* The resume reports V's failure and names the port. */
	error = drv_pci_resume_all();
	check(error == EIO, "port lost: EIO");
	check(strstr(klog, "ffff:e0:06.0 does not answer after the sleep") != NULL, "port lost: logged");
}

/*
 * The drivers of N and C, and the platform.
 */

/* Suspends N or C; C refuses when asked. */
static int
driver_suspend(
	struct drv_pci_device *device)
{
	/* Notes it. */
	note("suspend:%s", device->name);

	/* C refuses when the test asks. */
	if (device == &c_function && c_refuses)
		return EBUSY;

	/* Succeeded. */
	return 0;
}

/* Resumes N or C. */
static int
driver_resume(
	struct drv_pci_device *device)
{
	/* Notes it. */
	note("resume:%s", device->name);

	/* Succeeded. */
	return 0;
}

/* Notes the platform following a function. */
static int
platform_set_state(
	void *argument,
	struct drv_pci_device *device,
	unsigned state)
{
	(void)argument;

	/* Notes it. */
	note("platform:%s:%u", device->name, state);

	/* Succeeded. */
	return 0;
}

/*
 * The simulated PCI core.
 */

/* Visits the functions in the walk's order. */
int
drv_pci_foreach_device(
	drv_pci_device_iterator_t visitor,
	void *argument)
{
	unsigned index;
	int decision;

	/* Visits each function. */
	for (index = 0; index < sizeof(walk_order) / sizeof(walk_order[0]); index++) {
		/* Stops when the visitor asks. */
		decision = visitor(walk_order[index], argument);
		if (decision != 0)
			return decision;
	}

	/* Succeeded. */
	return 0;
}

/* Reports a function's driver. */
struct drv_pci_driver *
drv_pci_device_driver(
	const struct drv_pci_device *device)
{
	return device->driver;
}

/* Visits the functions of a bus: R on V's bus, N on R's. */
int
drv_pci_bus_foreach_device(
	struct drv_pci_bus *bus,
	drv_pci_device_iterator_t visitor,
	void *argument)
{
	/* The one function on each of the two buses behind V. */
	if (bus == &vmd_bus)
		return visitor(&r_function, argument);
	if (bus == &port_bus)
		return visitor(&n_function, argument);

	/* The platform's bus is not asked for. */
	unexpected(__func__);
	return EIO;
}

/* Reports a function's driver data. */
void *
drv_pci_device_driver_data(
	const struct drv_pci_device *device)
{
	return device->driver_data;
}

/* Reports the function above a function. */
struct drv_pci_device *
drv_pci_device_upstream(
	const struct drv_pci_device *device)
{
	/* A bus behind a bridge, then a root bus a function presents. */
	if (device->bus->bridge != NULL)
		return device->bus->bridge;
	return device->bus->host_function;
}

/* Reports a function's address. */
void
drv_pci_device_address(
	const struct drv_pci_device *device,
	struct drv_pci_address *address)
{
	*address = device->address;
}

/* Reads a byte. */
int
drv_pci_device_config_read8(
	struct drv_pci_device *device,
	unsigned offset,
	uint8_t *value)
{
	*value = (uint8_t)sim_read(device, offset, 1);
	return 0;
}

/* Reads a word. */
int
drv_pci_device_config_read16(
	struct drv_pci_device *device,
	unsigned offset,
	uint16_t *value)
{
	*value = (uint16_t)sim_read(device, offset, 2);
	return 0;
}

/* Reads a dword. */
int
drv_pci_device_config_read32(
	struct drv_pci_device *device,
	unsigned offset,
	uint32_t *value)
{
	*value = sim_read(device, offset, 4);
	return 0;
}

/* Writes a word. */
int
drv_pci_device_config_write16(
	struct drv_pci_device *device,
	unsigned offset,
	uint16_t value)
{
	sim_write(device, offset, 2, value);
	return 0;
}

/* Writes a dword. */
int
drv_pci_device_config_write32(
	struct drv_pci_device *device,
	unsigned offset,
	uint32_t value)
{
	sim_write(device, offset, 4, value);
	return 0;
}

/* Finds a capability in the list, as the function answers. */
int
drv_pci_device_find_capability(
	struct drv_pci_device *device,
	uint8_t id,
	unsigned *result)
{
	unsigned place;
	unsigned hops;

	/* Walks the list from the pointer. */
	place = sim_read(device, 0x34, 1);
	for (hops = 0; place != 0 && place != 0xffU && hops < 48U; hops++) {
		/* Reports the one asked for. */
		if (sim_read(device, place, 1) == id) {
			*result = place;
			return 0;
		}
		place = sim_read(device, place + 1U, 1);
	}

	/* Not there. */
	return ENOENT;
}

/* Finds LTR on N and C; nothing else has extended capabilities. */
int
drv_pci_device_find_extended_capability(
	struct drv_pci_device *device,
	uint16_t id,
	unsigned start,
	unsigned *result)
{
	(void)start;

	/* Only N and C have LTR. */
	if (id != 0x0018U || (device != &n_function && device != &c_function))
		return ENOENT;

	/* Reports it. */
	*result = E_LTR;
	return 0;
}

/* Maps a function's MSI-X table: the array in its record. */
int
drv_pci_device_map_msix_table(
	struct drv_pci_device *device,
	struct drv_pci_mapping *mapping,
	unsigned *entries)
{
	/* R and P have none. */
	if (device == &r_function || device == &p_function)
		return ENOENT;

	/* Describes the array. */
	memset(mapping, 0, sizeof(*mapping));
	mapping->address = device->msix;
	mapping->size = sizeof(device->msix);
	*entries = MSIX_ENTRIES;
	return 0;
}

/* Lets a mapping go: nothing to do. */
void
drv_pci_device_unmap_bar(
	struct drv_pci_device *device,
	struct drv_pci_mapping *mapping)
{
	(void)device;
	(void)mapping;
}

/* Reports V's BARs as its registers hold them now. */
int
drv_pci_device_bar(
	const struct drv_pci_device *device,
	unsigned index,
	struct drv_pci_bar *bar)
{
	uint64_t low;
	uint64_t high;

	/* Reads the 64-bit BAR. */
	low = get(device->config, 0x10U + index * 4U, 4) & ~UINT64_C(0xf);
	high = get(device->config, 0x14U + index * 4U, 4);
	memset(bar, 0, sizeof(*bar));
	bar->index = index;
	bar->type = DRV_PCI_BAR_MEMORY64;
	bar->bus_address = low | (high << 32);
	return 0;
}

/* The core never reattaches here. */
int
drv_pci_device_reprobe(
	struct drv_pci_device *device)
{
	(void)device;
	return ENOTSUP;
}

/*
 * The kernel services the code calls.
 */

/* Allocates. */
void *
kern_malloc(
	size_t size)
{
	return malloc(size);
}

/* Frees. */
void
kern_free(
	void *pointer)
{
	free(pointer);
}

/* Keeps the log. */
void
kern_logf(
	const char *format,
	...)
{
	va_list arguments;
	size_t used;

	/* Appends the line. */
	used = strlen(klog);
	va_start(arguments, format);
	vsnprintf(klog + used, LOG_MAX - used, format, arguments);
	va_end(arguments);
}

/* The tick counter stays at zero; a tick is a millisecond. */
uint64_t
sched_ticks(void)
{
	return 0;
}

/* Milliseconds as ticks. */
uint64_t
kern_ms_to_ticks(
	uint64_t milliseconds)
{
	return milliseconds;
}

/* A sleep returns at once and is counted. */
void
sched_sleep(
	uint64_t timeout_tick)
{
	slept_ms += timeout_tick;
}

/* The locks do nothing on one thread. */
void
spin_init(
	struct spinlock *lock,
	enum lock_rank rank,
	const char *name)
{
	(void)lock;
	(void)rank;
	(void)name;
}

/* Takes no lock. */
unsigned long
spin_lock_irqsave(
	struct spinlock *lock)
{
	(void)lock;
	return 0;
}

/* Releases no lock. */
void
spin_unlock_irqrestore(
	struct spinlock *lock,
	unsigned long enabled)
{
	(void)lock;
	(void)enabled;
}

/* The CFGBAR's reads and writes are plain memory. */
uint8_t
kern_mmio_read8(
	const volatile void *address)
{
	return *(const volatile uint8_t *)address;
}

/* Reads a word. */
uint16_t
kern_mmio_read16(
	const volatile void *address)
{
	uint16_t value;

	memcpy(&value, (const void *)address, sizeof(value));
	return value;
}

/* Reads a dword. */
uint32_t
kern_mmio_read32(
	const volatile void *address)
{
	uint32_t value;

	memcpy(&value, (const void *)address, sizeof(value));
	return value;
}

/* Writes a byte. */
void
kern_mmio_write8(
	volatile void *address,
	uint8_t value)
{
	*(volatile uint8_t *)address = value;
}

/* Writes a word. */
void
kern_mmio_write16(
	volatile void *address,
	uint16_t value)
{
	memcpy((void *)address, &value, sizeof(value));
}

/* Writes a dword. */
void
kern_mmio_write32(
	volatile void *address,
	uint32_t value)
{
	memcpy((void *)address, &value, sizeof(value));
}

/* Orders nothing on the host. */
void
kern_io_barrier(void)
{
}

/*
 * What the attach and the interrupts use, which these checks never reach:
 * each reports that it was reached and stops the test.
 */

/* Stops the test at a call it does not expect. */
static void
unexpected(
	const char *name)
{
	printf("FAIL unexpected call of %s\n", name);
	exit(1);
}

int drv_pci_bus_create_root(uint16_t segment, uint8_t number, const struct drv_pci_bus_ops *ops, void *host, struct drv_dma_device *dma, struct drv_pci_bus **result)
{ (void)segment; (void)number; (void)ops; (void)host; (void)dma; (void)result; unexpected(__func__); return EIO; }
int drv_pci_bus_scan_tree(struct drv_pci_bus *bus)
{ (void)bus; unexpected(__func__); return EIO; }
int drv_pci_bus_set_host_function(struct drv_pci_bus *bus, struct drv_pci_device *function)
{ (void)bus; (void)function; unexpected(__func__); return EIO; }
int drv_pci_device_allocate_irqs(struct drv_pci_device *d, unsigned flags, unsigned minimum, unsigned maximum, struct drv_pci_irq *irqs, unsigned *count)
{ (void)d; (void)flags; (void)minimum; (void)maximum; (void)irqs; (void)count; unexpected(__func__); return EIO; }
int drv_pci_device_claim_bar(struct drv_pci_device *d, unsigned index)
{ (void)d; (void)index; unexpected(__func__); return EIO; }
void drv_pci_device_disestablish_irq(struct drv_pci_device *d, void *cookie)
{ (void)d; (void)cookie; unexpected(__func__); }
struct drv_dma_device *drv_pci_device_dma(struct drv_pci_device *d)
{ (void)d; unexpected(__func__); return NULL; }
int drv_pci_device_enable_memory(struct drv_pci_device *d)
{ (void)d; unexpected(__func__); return EIO; }
int drv_pci_device_establish_irq(struct drv_pci_device *d, const struct drv_pci_irq *irq, int (*handler)(void *), void *argument, const char *name, void **cookie)
{ (void)d; (void)irq; (void)handler; (void)argument; (void)name; (void)cookie; unexpected(__func__); return EIO; }
void drv_pci_device_free_irqs(struct drv_pci_device *d, struct drv_pci_irq *irqs, unsigned count)
{ (void)d; (void)irqs; (void)count; unexpected(__func__); }
int drv_pci_device_map_bar(struct drv_pci_device *d, unsigned index, unsigned flags, struct drv_pci_mapping *mapping)
{ (void)d; (void)index; (void)flags; (void)mapping; unexpected(__func__); return EIO; }
void drv_pci_device_release_bar(struct drv_pci_device *d, unsigned index)
{ (void)d; (void)index; unexpected(__func__); }
int drv_pci_device_set_bus_master(struct drv_pci_device *d, bool enable)
{ (void)d; (void)enable; unexpected(__func__); return EIO; }
int drv_pci_device_set_driver_data(struct drv_pci_device *d, void *data)
{ (void)d; (void)data; unexpected(__func__); return EIO; }
int drv_pci_driver_register(struct drv_pci_driver *driver)
{ (void)driver; unexpected(__func__); return EIO; }
void *kern_boot_handoff(const char *name)
{ (void)name; unexpected(__func__); return NULL; }
int kern_boot_parameters_token_present(const char *text, const char *token)
{ (void)text; (void)token; unexpected(__func__); return 0; }
int kern_device_map(uint64_t address, size_t size, unsigned attributes, void **mapped)
{ (void)address; (void)size; (void)attributes; (void)mapped; unexpected(__func__); return EIO; }
int kern_device_unmap(void *mapped, size_t size)
{ (void)mapped; (void)size; unexpected(__func__); return EIO; }
void kern_usleep_range(unsigned min_us, unsigned max_us)
{ (void)min_us; (void)max_us; unexpected(__func__); }
