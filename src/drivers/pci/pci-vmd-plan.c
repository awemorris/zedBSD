/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The hardware-free half of the Intel VMD driver.
 *
 * It decides the bus numbers of the root ports behind a VMD, closes and
 * reopens their windows, sizes and places the BARs of the functions
 * behind them inside the VMD's memory windows, and keeps the table of the
 * children's forwarded message interrupts.  Configuration space is
 * reached only through the callbacks it is given, so a host test compiles
 * this file as it is and runs it against a model of the space.
 */

#include <drivers/pci/pci-vmd.h>
#include <drivers/pci/pci-window.h>
#include <uapi/errno.h>

#include <stddef.h>

/*
 * The registers of a configuration header this file reads and writes: the
 * vendor and device identity, the command register and its I/O, memory
 * and bus-master enables, the status register and its capability-list
 * bit, the class code, the header type and its multi-function bit, the
 * first BAR, and the capability pointer.
 *
 * Values referenced: PCI Local Bus Specification Revision 3.0, section 6.2
 * (the type 0 and type 1 header layouts and the command and status bits).
 */
#define VMD_CFG_ID			0x00U
#define VMD_CFG_COMMAND			0x04U
#define VMD_CFG_COMMAND_IO		0x0001U
#define VMD_CFG_COMMAND_MEMORY		0x0002U
#define VMD_CFG_COMMAND_MASTER		0x0004U
#define VMD_CFG_STATUS			0x06U
#define VMD_CFG_STATUS_CAPABILITIES	0x0010U
#define VMD_CFG_CLASS			0x08U
#define VMD_CFG_HEADER_TYPE		0x0eU
#define VMD_CFG_HEADER_MULTI		0x80U
#define VMD_CFG_HEADER_KIND		0x7fU
#define VMD_CFG_HEADER_BRIDGE		0x01U
#define VMD_CFG_BAR0			0x10U
#define VMD_CFG_CAPABILITY_POINTER	0x34U

/* The class code of a PCI-to-PCI bridge, in the upper half of the class register. */
#define VMD_CLASS_BRIDGE		0x0604U

/* The BARs of a type 0 header and of a type 1 header. */
#define VMD_ENDPOINT_BARS		6U
#define VMD_BRIDGE_BARS			2U

/*
 * The bits of a BAR: an I/O BAR, a memory BAR's type field and its 64-bit
 * value, the prefetchable bit, and the address masks of each kind.
 *
 * Values referenced: PCI Local Bus Specification Revision 3.0, section
 * 6.2.5.1 (the base address register layout).
 */
#define VMD_BAR_IO			0x00000001U
#define VMD_BAR_TYPE_SHIFT		1U
#define VMD_BAR_TYPE_MASK		0x3U
#define VMD_BAR_TYPE_64			0x2U
#define VMD_BAR_PREFETCHABLE		0x00000008U
#define VMD_BAR_MEMORY_MASK		0xfffffff0U

/*
 * The registers of a type 1 header: the primary, secondary and subordinate
 * bus numbers, the I/O base and limit, the memory base and limit, the
 * prefetchable base and limit, their upper halves, and the upper halves of
 * the I/O window.
 *
 * Values referenced: PCI-to-PCI Bridge Architecture Specification Revision
 * 1.2, section 3.2 (the type 1 configuration header).
 */
#define VMD_BRIDGE_PRIMARY		0x18U
#define VMD_BRIDGE_SECONDARY		0x19U
#define VMD_BRIDGE_SUBORDINATE		0x1aU
#define VMD_BRIDGE_IO_WINDOW		0x1cU
#define VMD_BRIDGE_MEMORY_WINDOW	0x20U
#define VMD_BRIDGE_PREFETCH_WINDOW	0x24U
#define VMD_BRIDGE_PREFETCH_BASE_UPPER	0x28U
#define VMD_BRIDGE_PREFETCH_LIMIT_UPPER	0x2cU
#define VMD_BRIDGE_IO_UPPER		0x30U

/*
 * The values that close every window of a root port, so that it decodes
 * nothing until it is given a window again: each base above its limit.
 *
 * Values referenced: Linux 6.12.5 drivers/pci/controller/vmd.c
 * (vmd_domain_reset, the values written to the I/O, memory and
 * prefetchable base and limit registers).
 */
#define VMD_CLOSE_IO_UPPER_HOLD		0x0000ffffU
#define VMD_CLOSE_IO_WINDOW		0x00f0U
#define VMD_CLOSE_MEMORY_WINDOW		0x0000fff0U
#define VMD_CLOSE_PREFETCH_BASE_UPPER	0xffffffffU

/*
 * A bridge memory window: its granule, and the place of the base's and the
 * limit's address bits 31:20 in the register.
 *
 * Values referenced: PCI-to-PCI Bridge Architecture Specification Revision
 * 1.2, section 3.2.5.8 (the memory base and memory limit registers).
 */
#define VMD_WINDOW_GRANULE		UINT64_C(0x100000)
#define VMD_WINDOW_FIELD_SHIFT		16U
#define VMD_WINDOW_FIELD_MASK		0xfff0U
#define VMD_WINDOW_LIMIT_SHIFT		16U

/*
 * The capabilities a plan quiets or reads: power management and its
 * control and status register, MSI and its enable, PCI Express with its
 * slot control (command completed and hot-plug interrupt enables) and
 * root control (PME interrupt enable), and MSI-X and its enable.
 *
 * Values referenced: PCI Local Bus Specification Revision 3.0, sections
 * 6.7 and 6.8 (the capability list, MSI and MSI-X); PCI Bus Power
 * Management Interface Specification 1.2, section 3.2.4 (PMCSR); PCI
 * Express Base Specification Revision 4.0, sections 7.5.3.10 and
 * 7.5.3.12 (slot control and root control).
 */
#define VMD_CAP_POWER			0x01U
#define VMD_CAP_POWER_CONTROL		0x04U
#define VMD_CAP_POWER_STATE_MASK	0x0003U
#define VMD_CAP_MSI			0x05U
#define VMD_CAP_MSI_CONTROL		0x02U
#define VMD_CAP_MSI_ENABLE		0x0001U
#define VMD_CAP_EXPRESS			0x10U
#define VMD_CAP_EXPRESS_SLOT_CONTROL	0x18U
#define VMD_CAP_EXPRESS_SLOT_INTERRUPTS	0x0030U
#define VMD_CAP_EXPRESS_ROOT_CONTROL	0x1cU
#define VMD_CAP_EXPRESS_ROOT_PME	0x0008U
#define VMD_CAP_MSIX			0x11U
#define VMD_CAP_MSIX_CONTROL		0x02U
#define VMD_CAP_MSIX_ENABLE		0x8000U

/* The most capabilities followed in one list, so that a looped list ends. */
#define VMD_CAPABILITIES_MAX		48U

/* The slots of a bus and the functions of a slot. */
#define VMD_SLOTS			32U
#define VMD_FUNCTIONS			8U

/*
 * Configuration space: one bus takes 1 MiB of the CFGBAR, one device
 * 32 KiB, one function 4 KiB.
 *
 * Values referenced: FreeBSD 15.0 sys/dev/vmd/vmd.c (vmd_read_config,
 * the offset of a register in the CFGBAR); PCI Express Base Specification
 * Revision 4.0, section 7.2.2 (the ECAM address).
 */
#define VMD_CONFIG_BUS_SHIFT		20U
#define VMD_CONFIG_DEVICE_SHIFT		15U
#define VMD_CONFIG_FUNCTION_SHIFT	12U
#define VMD_CONFIG_SPACE		4096U

/*
 * The VMD's own registers that place its buses: the capability register
 * and its bus-restriction bit, and the configuration register's bus-start
 * field with the three starts it selects.
 *
 * Values referenced: FreeBSD 15.0 sys/dev/vmd/vmd.c (VMD_CAP,
 * VMD_BUS_RESTRICT, VMD_BUS_START and the starts 0, 128 and 224 in
 * vmd_attach).
 */
#define VMD_CAPABILITY_BUS_RESTRICT	0x0001U
#define VMD_CONFIGURATION_BUS_SHIFT	8U
#define VMD_CONFIGURATION_BUS_MASK	0x3U
#define VMD_BUS_START_LOW		0U
#define VMD_BUS_START_MIDDLE		128U
#define VMD_BUS_START_HIGH		224U

/*
 * The message a child writes to raise a VMD vector: the processor's
 * message address with the VMD vector in bits 19:12, and no data.
 *
 * Values referenced: FreeBSD 15.0 sys/dev/vmd/vmd.c (vmd_map_msi).
 */
#define VMD_MESSAGE_ADDRESS		UINT64_C(0xfee00000)
#define VMD_MESSAGE_VECTOR_SHIFT	12U

/* The least space one BAR takes in a window, so that no two share a page. */
#define VMD_BAR_MIN_SPAN		UINT64_C(0x1000)

static int config_read(const struct drv_pci_vmd_config_access *access, unsigned bus, unsigned device, unsigned function, unsigned offset, unsigned width, uint32_t *value);
static int config_write(const struct drv_pci_vmd_config_access *access, unsigned bus, unsigned device, unsigned function, unsigned offset, unsigned width, uint32_t value);
static bool function_present(const struct drv_pci_vmd_config_access *access, unsigned bus, unsigned device, unsigned function);
static unsigned slot_functions(const struct drv_pci_vmd_config_access *access, unsigned bus, unsigned device);
static bool function_is_bridge(const struct drv_pci_vmd_config_access *access, unsigned bus, unsigned device, unsigned function);
static int find_capability(const struct drv_pci_vmd_config_access *access, unsigned bus, unsigned device, unsigned function, unsigned id, unsigned *position);
static void clear_bits16(const struct drv_pci_vmd_config_access *access, unsigned bus, unsigned device, unsigned function, unsigned offset, uint32_t bits);
static void quiet_function(const struct drv_pci_vmd_config_access *access, unsigned bus, unsigned device, unsigned function);
static void quiet_port_messages(const struct drv_pci_vmd_config_access *access, unsigned bus, struct drv_pci_vmd_port *port);
static void close_port_windows(const struct drv_pci_vmd_config_access *access, unsigned bus, struct drv_pci_vmd_port *port);
static void write_port_buses(const struct drv_pci_vmd_config_access *access, unsigned bus, struct drv_pci_vmd_port *port, unsigned primary, unsigned secondary);
static int size_bar(const struct drv_pci_vmd_config_access *access, unsigned bus, unsigned device, unsigned function, unsigned index, uint64_t *size, bool *wide, bool *prefetchable);
static bool port_has_own_bar(const struct drv_pci_vmd_config_access *access, unsigned bus, struct drv_pci_vmd_port *port);
static void survey_endpoint(const struct drv_pci_vmd_config_access *access, struct drv_pci_vmd_port *port, unsigned device, unsigned function);
static void survey_below(const struct drv_pci_vmd_config_access *access, struct drv_pci_vmd_port *port);
static void order_bars(const struct drv_pci_vmd_port *port, unsigned *order);
static uint64_t bar_span(const struct drv_pci_vmd_bar *bar);
static void place_port(const struct drv_pci_vmd_domain *domain, struct drv_pci_vmd_plan *plan, struct drv_pci_vmd_port *port);
static void write_port(const struct drv_pci_vmd_config_access *access, unsigned bus, struct drv_pci_vmd_port *port);
static void retire_port(const struct drv_pci_vmd_config_access *access, unsigned bus, struct drv_pci_vmd_port *port);
static void write_bar(const struct drv_pci_vmd_config_access *access, unsigned bus, const struct drv_pci_vmd_bar *bar, uint64_t address);

/*
 * Reports the first bus number a VMD's CFGBAR decodes.
 *
 * A VMD without the bus restriction starts at bus 0; one with it starts
 * where its configuration register says.  EINVAL reports a setting with
 * no known start.
 */
int
drv_pci_vmd_bus_start(
	uint16_t capability,
	uint16_t configuration,
	unsigned *bus_start)
{
	unsigned field;

	/* Refuses missing storage. */
	if (bus_start == NULL)
		return EINVAL;

	/* A VMD that cannot restrict its buses starts at bus 0. */
	if ((capability & VMD_CAPABILITY_BUS_RESTRICT) == 0) {
		*bus_start = VMD_BUS_START_LOW;
		return 0;
	}

	/* Reads the start the configuration register selects. */
	field = ((unsigned)configuration >> VMD_CONFIGURATION_BUS_SHIFT) & VMD_CONFIGURATION_BUS_MASK;

	/* Turns the field into the bus number. */
	switch (field) {
	case 0:
		*bus_start = VMD_BUS_START_LOW;
		break;
	case 1:
		*bus_start = VMD_BUS_START_MIDDLE;
		break;
	case 2:
		*bus_start = VMD_BUS_START_HIGH;
		break;
	default:
		/* The fourth setting names no start. */
		return EINVAL;
	}

	/* Succeeded: the first bus. */
	return 0;
}

/*
 * Finds the offset of one register in a VMD's CFGBAR.
 *
 * EINVAL reports an access the CFGBAR does not decode: a bus outside the
 * VMD's range, a device or function that does not exist, a width other
 * than 1, 2 or 4, an access that is not aligned to its width or that
 * crosses the end of a function's space or of the CFGBAR.
 */
int
drv_pci_vmd_config_offset(
	unsigned bus_start,
	unsigned bus_end,
	uint64_t cfgbar_size,
	unsigned bus,
	unsigned device,
	unsigned function,
	unsigned offset,
	unsigned width,
	uint64_t *result)
{
	uint64_t position;

	/* Refuses missing storage and a bus the VMD does not decode. */
	if (result == NULL)
		return EINVAL;
	if (bus < bus_start || bus > bus_end)
		return EINVAL;

	/* Refuses a device, function or width configuration space does not have. */
	if (device >= VMD_SLOTS || function >= VMD_FUNCTIONS)
		return EINVAL;
	if (width != 1U && width != 2U && width != 4U)
		return EINVAL;

	/* Refuses an access that is unaligned or leaves the function's space. */
	if ((offset & (width - 1U)) != 0)
		return EINVAL;
	if (offset > VMD_CONFIG_SPACE - width)
		return EINVAL;

	/* Computes the register's place in the CFGBAR. */
	position = (uint64_t)(bus - bus_start) << VMD_CONFIG_BUS_SHIFT;
	position += (uint64_t)device << VMD_CONFIG_DEVICE_SHIFT;
	position += (uint64_t)function << VMD_CONFIG_FUNCTION_SHIFT;
	position += offset;

	/* Refuses a register past the end of the CFGBAR. */
	if (cfgbar_size < width || position > cfgbar_size - width)
		return EINVAL;

	/* Succeeded: the register's offset. */
	*result = position;
	return 0;
}

/*
 * Reports what configuration space reads as where nothing answers.
 */
uint32_t
drv_pci_vmd_absent_value(
	unsigned width)
{
	/* A byte, a word or a double word of all ones. */
	if (width == 1U)
		return 0xffU;
	if (width == 2U)
		return 0xffffU;

	/* Reports a whole register of ones. */
	return 0xffffffffU;
}

/*
 * Numbers, closes and places everything behind a VMD.
 *
 * The root ports on the VMD's first bus get their bus numbers, the
 * functions behind them their BARs inside the VMD's windows, and the
 * ports the windows that hold those BARs.  Every function is left with
 * its memory, I/O and bus-master enables off, except the ports in use,
 * which decode their windows and forward their children's requests.  A
 * port that cannot be used is switched off whole and says why in the
 * plan.
 */
int
drv_pci_vmd_plan_domain(
	const struct drv_pci_vmd_config_access *access,
	const struct drv_pci_vmd_domain *domain,
	struct drv_pci_vmd_plan *plan)
{
	struct drv_pci_vmd_port *port;
	unsigned device;
	unsigned function;
	unsigned functions;
	unsigned next_bus;
	unsigned index;
	bool present;
	bool bridge;
	bool own_bar;

	/* Refuses missing arguments and a domain with no bus behind its first. */
	if (access == NULL || access->read == NULL || access->write == NULL)
		return EINVAL;
	if (domain == NULL || plan == NULL)
		return EINVAL;
	if (domain->bus_start > domain->bus_end || domain->bus_end > 255U)
		return EINVAL;
	if (domain->window_count > DRV_PCI_VMD_WINDOWS_MAX)
		return EINVAL;

	/* Starts with no port, no endpoint and nothing dropped. */
	plan->port_count = 0;
	plan->endpoints = 0;
	plan->ports_dropped = 0;

	/* Finds the functions on the first bus, switching each one off. */
	for (device = 0; device < VMD_SLOTS; device++) {
		functions = slot_functions(access, domain->bus_start, device);
		for (function = 0; function < functions; function++) {
			/* Passes over a function that does not answer. */
			present = function_present(access, domain->bus_start, device, function);
			if (!present)
				continue;

			/* Stops the function decoding and mastering while it is rearranged. */
			quiet_function(access, domain->bus_start, device, function);

			/* Counts a function that is not a root port; it stays off. */
			bridge = function_is_bridge(access, domain->bus_start, device, function);
			if (!bridge) {
				plan->endpoints++;
				continue;
			}

			/* Counts a root port beyond the plan's room; it stays off. */
			if (plan->port_count == DRV_PCI_VMD_PORTS_MAX) {
				plan->ports_dropped++;
				continue;
			}

			/* Records the root port. */
			port = &plan->ports[plan->port_count];
			port->device = device;
			port->function = function;
			port->state = DRV_PCI_VMD_PORT_USED;
			port->secondary = 0;
			port->window_base = 0;
			port->window_length = 0;
			port->firmware_buses = 0;
			port->firmware_window = 0;
			port->bar_count = 0;
			plan->port_count++;
		}
	}

	/* Silences each port and closes its windows before anything is placed. */
	for (index = 0; index < plan->port_count; index++) {
		port = &plan->ports[index];
		quiet_port_messages(access, domain->bus_start, port);
		close_port_windows(access, domain->bus_start, port);

		/* A port that decodes BARs of its own is more than this driver handles. */
		own_bar = port_has_own_bar(access, domain->bus_start, port);
		if (own_bar)
			port->state = DRV_PCI_VMD_PORT_OWN_BAR;
	}

	/* Gives each port in use the next bus number, and finds what is behind it. */
	next_bus = domain->bus_start + 1U;
	for (index = 0; index < plan->port_count; index++) {
		port = &plan->ports[index];
		if (port->state != DRV_PCI_VMD_PORT_USED)
			continue;

		/* A port past the last bus the VMD decodes cannot be given one. */
		if (next_bus > domain->bus_end) {
			port->state = DRV_PCI_VMD_PORT_NO_BUS;
			continue;
		}

		/* Numbers the port's secondary bus and looks behind it. */
		port->secondary = next_bus;
		next_bus++;
		write_port_buses(access, domain->bus_start, port, domain->bus_start, port->secondary);
		survey_below(access, port);
	}

	/* Places the window and the BARs of each port still in use. */
	for (index = 0; index < plan->port_count; index++) {
		port = &plan->ports[index];
		if (port->state != DRV_PCI_VMD_PORT_USED)
			continue;

		place_port(domain, plan, port);
	}

	/* Writes each port in use, and switches off every other one. */
	for (index = 0; index < plan->port_count; index++) {
		port = &plan->ports[index];
		if (port->state == DRV_PCI_VMD_PORT_USED) {
			write_port(access, domain->bus_start, port);
		} else {
			retire_port(access, domain->bus_start, port);
		}
	}

	/* Succeeded: the plan says what became of each port. */
	return 0;
}

/*
 * Reports the message a child writes to raise one VMD vector.
 */
void
drv_pci_vmd_message_address(
	unsigned vector,
	uint64_t *address,
	uint32_t *data)
{
	/* The vector rides in the destination field of the processor's address. */
	*address = VMD_MESSAGE_ADDRESS | ((uint64_t)vector << VMD_MESSAGE_VECTOR_SHIFT);

	/* The VMD ignores the data; it is written as zero. */
	*data = 0;
}

/*
 * Empties the table of forwarded message interrupts.
 */
void
drv_pci_vmd_messages_init(
	struct drv_pci_vmd_messages *messages)
{
	unsigned slot;

	/* Frees every slot. */
	for (slot = 0; slot < DRV_PCI_VMD_MESSAGES_MAX; slot++) {
		messages->slots[slot].run = NULL;
		messages->slots[slot].argument = NULL;
		messages->slots[slot].in_flight = 0;
	}
}

/*
 * Takes a free slot for one child's message interrupt.
 *
 * ENOSPC reports a table with no free slot: every slot is held, or still
 * being run by an interrupt that has not returned.
 */
int
drv_pci_vmd_message_take(
	struct drv_pci_vmd_messages *messages,
	drv_pci_vmd_run_t run,
	void *argument,
	unsigned *slot)
{
	struct drv_pci_vmd_message *message;
	unsigned index;

	/* Refuses a missing handler or storage. */
	if (messages == NULL || run == NULL || slot == NULL)
		return EINVAL;

	/* Looks for a slot nobody holds and no interrupt still runs. */
	for (index = 0; index < DRV_PCI_VMD_MESSAGES_MAX; index++) {
		message = &messages->slots[index];
		if (message->run == NULL && message->in_flight == 0)
			break;
	}

	/* Every slot is taken. */
	if (index == DRV_PCI_VMD_MESSAGES_MAX)
		return ENOSPC;

	/* Fills the slot. */
	message->run = run;
	message->argument = argument;

	/* Succeeded: the slot names the message from now on. */
	*slot = index;
	return 0;
}

/*
 * Empties one slot and reports what it held, so that it can be put back.
 *
 * EINVAL reports a slot that does not exist or holds nothing.
 */
int
drv_pci_vmd_message_clear(
	struct drv_pci_vmd_messages *messages,
	unsigned slot,
	drv_pci_vmd_run_t *run,
	void **argument)
{
	struct drv_pci_vmd_message *message;

	/* Refuses a slot the table does not have or that is free. */
	if (messages == NULL || run == NULL || argument == NULL)
		return EINVAL;
	if (slot >= DRV_PCI_VMD_MESSAGES_MAX)
		return EINVAL;
	message = &messages->slots[slot];
	if (message->run == NULL)
		return EINVAL;

	/* Reports what the slot held and frees it. */
	*run = message->run;
	*argument = message->argument;
	message->run = NULL;
	message->argument = NULL;

	/* Succeeded: no new interrupt runs the slot. */
	return 0;
}

/*
 * Puts back what a slot held before a clear that could not be completed.
 */
void
drv_pci_vmd_message_restore(
	struct drv_pci_vmd_messages *messages,
	unsigned slot,
	drv_pci_vmd_run_t run,
	void *argument)
{
	/* Ignores a slot the table does not have. */
	if (messages == NULL || slot >= DRV_PCI_VMD_MESSAGES_MAX)
		return;

	/* Fills the slot again. */
	messages->slots[slot].run = run;
	messages->slots[slot].argument = argument;
}

/*
 * Reports how many interrupts are still running one slot.
 */
unsigned
drv_pci_vmd_message_in_flight(
	const struct drv_pci_vmd_messages *messages,
	unsigned slot)
{
	/* A slot the table does not have runs nothing. */
	if (messages == NULL || slot >= DRV_PCI_VMD_MESSAGES_MAX)
		return 0;

	/* Reports the slot's count. */
	return messages->slots[slot].in_flight;
}

/*
 * Takes out every held message for one interrupt to run.
 *
 * Each one is marked as running until drv_pci_vmd_messages_release() is
 * called with the same list, so that a clear can wait for it.  pending
 * has room for DRV_PCI_VMD_MESSAGES_MAX entries; the count is reported.
 */
unsigned
drv_pci_vmd_messages_collect(
	struct drv_pci_vmd_messages *messages,
	struct drv_pci_vmd_pending *pending)
{
	struct drv_pci_vmd_message *message;
	unsigned count;
	unsigned slot;

	/* Copies out each held slot and marks it running. */
	count = 0;
	for (slot = 0; slot < DRV_PCI_VMD_MESSAGES_MAX; slot++) {
		message = &messages->slots[slot];
		if (message->run == NULL)
			continue;

		/*
		 * The count keeps a clear of this slot waiting until the
		 * interrupt that took the handler out has returned from it.
		 */
		message->in_flight++;
		pending[count].slot = slot;
		pending[count].run = message->run;
		pending[count].argument = message->argument;
		count++;
	}

	/* Reports how many handlers the interrupt is to run. */
	return count;
}

/*
 * Marks the messages one interrupt ran as no longer running.
 */
void
drv_pci_vmd_messages_release(
	struct drv_pci_vmd_messages *messages,
	const struct drv_pci_vmd_pending *pending,
	unsigned count)
{
	unsigned index;

	/* Gives back the running mark of each slot the interrupt took. */
	for (index = 0; index < count; index++) {
		/* A clear waiting on the slot may finish once its count is zero. */
		messages->slots[pending[index].slot].in_flight--;
	}
}

/* Reads one register through the access callbacks, as all ones when that fails. */
static int
config_read(
	const struct drv_pci_vmd_config_access *access,
	unsigned bus,
	unsigned device,
	unsigned function,
	unsigned offset,
	unsigned width,
	uint32_t *value)
{
	int error;

	/* Asks the callback for the register. */
	error = access->read(access->context, bus, device, function, offset, width, value);

	/* A failed access reads as if nothing answered. */
	if (error != 0) {
		*value = drv_pci_vmd_absent_value(width);
		return error;
	}

	/* Succeeded: the register's value. */
	return 0;
}

/* Writes one register through the access callbacks. */
static int
config_write(
	const struct drv_pci_vmd_config_access *access,
	unsigned bus,
	unsigned device,
	unsigned function,
	unsigned offset,
	unsigned width,
	uint32_t value)
{
	int error;

	/* Hands the register to the callback. */
	error = access->write(access->context, bus, device, function, offset, width, value);
	if (error != 0)
		return error;

	/* Succeeded: the register is written. */
	return 0;
}

/* Tells whether a function answers configuration reads. */
static bool
function_present(
	const struct drv_pci_vmd_config_access *access,
	unsigned bus,
	unsigned device,
	unsigned function)
{
	uint32_t identity;
	int error;

	/* Reads the vendor and device identity. */
	error = config_read(access, bus, device, function, VMD_CFG_ID, 4U, &identity);
	if (error != 0)
		return false;

	/* An all-ones vendor is nobody. */
	if ((identity & 0xffffU) == 0xffffU)
		return false;

	/* Succeeded: something answers. */
	return true;
}

/* Reports how many functions of a slot to look at: eight for a multi-function device. */
static unsigned
slot_functions(
	const struct drv_pci_vmd_config_access *access,
	unsigned bus,
	unsigned device)
{
	uint32_t header;
	bool present;
	int error;

	/* A slot whose first function is absent has no other function. */
	present = function_present(access, bus, device, 0);
	if (!present)
		return 1U;

	/* Reads the first function's header type. */
	error = config_read(access, bus, device, 0, VMD_CFG_HEADER_TYPE, 1U, &header);
	if (error != 0)
		return 1U;

	/* A multi-function device may answer on all eight. */
	if ((header & VMD_CFG_HEADER_MULTI) != 0)
		return VMD_FUNCTIONS;

	/* Reports the single function. */
	return 1U;
}

/* Tells whether a function is a PCI-to-PCI bridge (a type 1 header of the bridge class). */
static bool
function_is_bridge(
	const struct drv_pci_vmd_config_access *access,
	unsigned bus,
	unsigned device,
	unsigned function)
{
	uint32_t header;
	uint32_t class_code;
	int error;

	/* Reads the header type; only a type 1 header can be a bridge. */
	error = config_read(access, bus, device, function, VMD_CFG_HEADER_TYPE, 1U, &header);
	if (error != 0)
		return false;
	if ((header & VMD_CFG_HEADER_KIND) != VMD_CFG_HEADER_BRIDGE)
		return false;

	/* Reads the class code, whose upper half names a PCI-to-PCI bridge. */
	error = config_read(access, bus, device, function, VMD_CFG_CLASS, 4U, &class_code);
	if (error != 0)
		return false;
	if ((class_code >> 16) != VMD_CLASS_BRIDGE)
		return false;

	/* Succeeded: the function is a bridge. */
	return true;
}

/* Finds a capability in a function's list; ENOENT reports none. */
static int
find_capability(
	const struct drv_pci_vmd_config_access *access,
	unsigned bus,
	unsigned device,
	unsigned function,
	unsigned id,
	unsigned *position)
{
	uint32_t status;
	uint32_t entry;
	unsigned pointer;
	unsigned steps;
	int error;

	/* A function without the list bit has no capabilities. */
	error = config_read(access, bus, device, function, VMD_CFG_STATUS, 2U, &status);
	if (error != 0)
		return error;
	if ((status & VMD_CFG_STATUS_CAPABILITIES) == 0)
		return ENOENT;

	/* Reads where the list starts. */
	error = config_read(access, bus, device, function, VMD_CFG_CAPABILITY_POINTER, 1U, &entry);
	if (error != 0)
		return error;

	/* Follows the list, a bounded number of steps, until the capability turns up. */
	pointer = entry & 0xfcU;
	for (steps = 0; steps < VMD_CAPABILITIES_MAX; steps++) {
		/* The end of the list, or a pointer into the header, ends the search. */
		if (pointer < 0x40U)
			break;

		/* Reads the capability's identity and the pointer to the next one. */
		error = config_read(access, bus, device, function, pointer, 2U, &entry);
		if (error != 0)
			return error;

		/* Reports the capability once its identity matches. */
		if ((entry & 0xffU) == id) {
			*position = pointer;
			return 0;
		}

		pointer = (entry >> 8) & 0xfcU;
	}

	/* The function does not have the capability. */
	return ENOENT;
}

/* Clears some bits of a 16-bit register, keeping the others. */
static void
clear_bits16(
	const struct drv_pci_vmd_config_access *access,
	unsigned bus,
	unsigned device,
	unsigned function,
	unsigned offset,
	uint32_t bits)
{
	uint32_t value;
	int error;

	/* Reads the register as it is. */
	error = config_read(access, bus, device, function, offset, 2U, &value);
	if (error != 0)
		return;

	/* Leaves a register that already has the bits clear untouched. */
	if ((value & bits) == 0)
		return;

	/* Writes it back without the bits. */
	(void)config_write(access, bus, device, function, offset, 2U, value & ~bits & 0xffffU);
}

/* Switches off a function's memory and I/O decoding and its bus mastering. */
static void
quiet_function(
	const struct drv_pci_vmd_config_access *access,
	unsigned bus,
	unsigned device,
	unsigned function)
{
	/* Clears the three enables of the command register. */
	clear_bits16(
		access,
		bus,
		device,
		function,
		VMD_CFG_COMMAND,
		VMD_CFG_COMMAND_IO | VMD_CFG_COMMAND_MEMORY | VMD_CFG_COMMAND_MASTER);
}

/*
 * Stops a root port raising message interrupts of its own.
 *
 * No driver serves a root port behind a VMD, so the MSI and MSI-X the
 * firmware may have left enabled, and the hot-plug and PME interrupts of
 * its slot and root control, are switched off.
 */
static void
quiet_port_messages(
	const struct drv_pci_vmd_config_access *access,
	unsigned bus,
	struct drv_pci_vmd_port *port)
{
	unsigned position;
	int error;

	/* Disables MSI. */
	error = find_capability(access, bus, port->device, port->function, VMD_CAP_MSI, &position);
	if (error == 0)
		clear_bits16(access, bus, port->device, port->function, position + VMD_CAP_MSI_CONTROL, VMD_CAP_MSI_ENABLE);

	/* Disables MSI-X. */
	error = find_capability(access, bus, port->device, port->function, VMD_CAP_MSIX, &position);
	if (error == 0)
		clear_bits16(access, bus, port->device, port->function, position + VMD_CAP_MSIX_CONTROL, VMD_CAP_MSIX_ENABLE);

	/* Disables the slot's hot-plug and command-completed interrupts and the root's PME interrupt. */
	error = find_capability(access, bus, port->device, port->function, VMD_CAP_EXPRESS, &position);
	if (error == 0) {
		clear_bits16(
			access,
			bus,
			port->device,
			port->function,
			position + VMD_CAP_EXPRESS_SLOT_CONTROL,
			VMD_CAP_EXPRESS_SLOT_INTERRUPTS);
		clear_bits16(
			access,
			bus,
			port->device,
			port->function,
			position + VMD_CAP_EXPRESS_ROOT_CONTROL,
			VMD_CAP_EXPRESS_ROOT_PME);
	}
}

/*
 * Closes every window of a root port, keeping what the firmware had left.
 *
 * Procedure referenced: Linux 6.12.5 drivers/pci/controller/vmd.c
 * (vmd_domain_reset, the order of the writes: the I/O upper halves are
 * held open around the 16-bit I/O base and limit, and the prefetchable
 * upper limit is cleared before the base is raised).
 */
static void
close_port_windows(
	const struct drv_pci_vmd_config_access *access,
	unsigned bus,
	struct drv_pci_vmd_port *port)
{
	unsigned device;
	unsigned function;

	/* Names the port's place on the first bus. */
	device = port->device;
	function = port->function;

	/* Keeps the firmware's bus numbers and memory window for the log. */
	(void)config_read(access, bus, device, function, VMD_BRIDGE_PRIMARY, 4U, &port->firmware_buses);
	(void)config_read(access, bus, device, function, VMD_BRIDGE_MEMORY_WINDOW, 4U, &port->firmware_window);

	/*
	 * Closes the I/O window: the upper halves first, so the base never
	 * passes below the limit through a half-written value, then the
	 * 16-bit base and limit (the status register beside them clears on a
	 * written one), then the upper halves back to zero.
	 */
	(void)config_write(access, bus, device, function, VMD_BRIDGE_IO_UPPER, 4U, VMD_CLOSE_IO_UPPER_HOLD);
	(void)config_write(access, bus, device, function, VMD_BRIDGE_IO_WINDOW, 2U, VMD_CLOSE_IO_WINDOW);
	(void)config_write(access, bus, device, function, VMD_BRIDGE_IO_UPPER, 4U, 0);

	/* Closes the memory window. */
	(void)config_write(access, bus, device, function, VMD_BRIDGE_MEMORY_WINDOW, 4U, VMD_CLOSE_MEMORY_WINDOW);

	/* Closes the prefetchable window: the upper limit, the window, then the upper base. */
	(void)config_write(access, bus, device, function, VMD_BRIDGE_PREFETCH_LIMIT_UPPER, 4U, 0);
	(void)config_write(access, bus, device, function, VMD_BRIDGE_PREFETCH_WINDOW, 4U, VMD_CLOSE_MEMORY_WINDOW);
	(void)config_write(access, bus, device, function, VMD_BRIDGE_PREFETCH_BASE_UPPER, 4U, VMD_CLOSE_PREFETCH_BASE_UPPER);
}

/* Writes a root port's primary, secondary and subordinate bus numbers. */
static void
write_port_buses(
	const struct drv_pci_vmd_config_access *access,
	unsigned bus,
	struct drv_pci_vmd_port *port,
	unsigned primary,
	unsigned secondary)
{
	/* The port's own bus, the bus behind it, and the last bus below it (the same one). */
	(void)config_write(access, bus, port->device, port->function, VMD_BRIDGE_PRIMARY, 1U, primary);
	(void)config_write(access, bus, port->device, port->function, VMD_BRIDGE_SECONDARY, 1U, secondary);
	(void)config_write(access, bus, port->device, port->function, VMD_BRIDGE_SUBORDINATE, 1U, secondary);
}

/*
 * Measures one BAR by writing all ones and reading back what sticks.
 *
 * A size of zero reports a BAR that is not implemented, or an I/O BAR,
 * which is cleared because a VMD forwards no I/O.  wide reports a 64-bit
 * BAR, whose upper half is the next register.  The function's decoding
 * must already be off.
 */
static int
size_bar(
	const struct drv_pci_vmd_config_access *access,
	unsigned bus,
	unsigned device,
	unsigned function,
	unsigned index,
	uint64_t *size,
	bool *wide,
	bool *prefetchable)
{
	uint32_t original;
	uint32_t low_mask;
	uint32_t high_mask;
	uint64_t mask;
	unsigned offset;
	int error;

	/* Starts from a BAR of no size, and finds its register. */
	*size = 0;
	*wide = false;
	*prefetchable = false;
	offset = VMD_CFG_BAR0 + index * 4U;

	/* Reads what the BAR holds now. */
	error = config_read(access, bus, device, function, offset, 4U, &original);
	if (error != 0)
		return error;

	/* Clears an I/O BAR, which nothing behind a VMD can use. */
	if ((original & VMD_BAR_IO) != 0) {
		(void)config_write(access, bus, device, function, offset, 4U, 0);
		return 0;
	}

	/* Notes the kind of memory BAR. */
	if (((original >> VMD_BAR_TYPE_SHIFT) & VMD_BAR_TYPE_MASK) == VMD_BAR_TYPE_64)
		*wide = true;
	if ((original & VMD_BAR_PREFETCHABLE) != 0)
		*prefetchable = true;

	/* Writes all ones to the low half and reads back the bits that stick. */
	(void)config_write(access, bus, device, function, offset, 4U, 0xffffffffU);
	(void)config_read(access, bus, device, function, offset, 4U, &low_mask);

	/* Does the same for the upper half of a 64-bit BAR. */
	high_mask = 0;
	if (*wide) {
		(void)config_write(access, bus, device, function, offset + 4U, 4U, 0xffffffffU);
		(void)config_read(access, bus, device, function, offset + 4U, 4U, &high_mask);
	}

	/* An unimplemented BAR keeps none of the address bits. */
	mask = ((uint64_t)high_mask << 32) | (low_mask & VMD_BAR_MEMORY_MASK);
	if (!*wide)
		mask &= 0xffffffffU;
	if (mask == 0)
		return 0;

	/*
	 * The size is the lowest address bit that sticks.  A function that
	 * implements fewer than 64 address bits reads zeros above them, so the
	 * inverted mask plus one would be wrong.
	 */
	*size = mask & (~mask + 1U);
	return 0;
}

/* Tells whether a root port decodes a memory BAR of its own. */
static bool
port_has_own_bar(
	const struct drv_pci_vmd_config_access *access,
	unsigned bus,
	struct drv_pci_vmd_port *port)
{
	uint64_t size;
	unsigned index;
	bool wide;
	bool prefetchable;
	bool found;
	int error;

	/* Measures the port's two BARs. */
	found = false;
	for (index = 0; index < VMD_BRIDGE_BARS; index++) {
		/* Sizes the BAR; one that cannot be read counts as one in use. */
		error = size_bar(access, bus, port->device, port->function, index, &size, &wide, &prefetchable);
		if (error != 0) {
			found = true;
			break;
		}

		/* Clears a BAR left all ones by the sizing and notes it. */
		if (size != 0) {
			(void)config_write(access, bus, port->device, port->function, VMD_CFG_BAR0 + index * 4U, 4U, 0);
			found = true;
		}

		/* A 64-bit BAR takes the next register too. */
		if (wide)
			index++;
	}

	/* Reports whether the port has a BAR. */
	return found;
}

/* Switches off one function behind a port, checks it, and records its memory BARs. */
static void
survey_endpoint(
	const struct drv_pci_vmd_config_access *access,
	struct drv_pci_vmd_port *port,
	unsigned device,
	unsigned function)
{
	struct drv_pci_vmd_bar *bar;
	uint32_t control;
	uint64_t size;
	unsigned position;
	unsigned index;
	bool wide;
	bool prefetchable;
	bool bridge;
	int error;

	/* Stops the function decoding and mastering while it is measured. */
	quiet_function(access, port->secondary, device, function);

	/* A switch behind the port is more than this driver handles. */
	bridge = function_is_bridge(access, port->secondary, device, function);
	if (bridge) {
		port->state = DRV_PCI_VMD_PORT_SWITCH;
		return;
	}

	/* A function that is not in D0 cannot be measured; the port is left alone. */
	error = find_capability(access, port->secondary, device, function, VMD_CAP_POWER, &position);
	if (error == 0) {
		error = config_read(access, port->secondary, device, function, position + VMD_CAP_POWER_CONTROL, 2U, &control);
		if (error == 0 && (control & VMD_CAP_POWER_STATE_MASK) != 0) {
			port->state = DRV_PCI_VMD_PORT_NOT_D0;
			return;
		}
	}

	/* Measures each BAR and records the memory ones. */
	for (index = 0; index < VMD_ENDPOINT_BARS; index++) {
		/* Sizes the BAR. */
		error = size_bar(access, port->secondary, device, function, index, &size, &wide, &prefetchable);
		if (error != 0)
			continue;

		/* Records a memory BAR that decodes something. */
		if (size != 0) {
			/* A port with more BARs than the plan has room for is left alone. */
			if (port->bar_count == DRV_PCI_VMD_PORT_BARS_MAX) {
				port->state = DRV_PCI_VMD_PORT_TOO_MANY_BARS;
				return;
			}

			bar = &port->bars[port->bar_count];
			bar->device = device;
			bar->function = function;
			bar->index = index;
			bar->size = size;
			bar->address = 0;
			bar->wide = wide;
			bar->prefetchable = prefetchable;
			port->bar_count++;
		}

		/* A 64-bit BAR takes the next register too. */
		if (wide)
			index++;
	}
}

/* Finds and measures the functions on a port's secondary bus. */
static void
survey_below(
	const struct drv_pci_vmd_config_access *access,
	struct drv_pci_vmd_port *port)
{
	unsigned device;
	unsigned function;
	unsigned functions;
	bool present;

	/* Looks at each function of each slot behind the port. */
	for (device = 0; device < VMD_SLOTS; device++) {
		functions = slot_functions(access, port->secondary, device);
		for (function = 0; function < functions; function++) {
			/* Passes over a function that does not answer. */
			present = function_present(access, port->secondary, device, function);
			if (!present)
				continue;

			/* Measures the function; stops once the port cannot be used. */
			survey_endpoint(access, port, device, function);
			if (port->state != DRV_PCI_VMD_PORT_USED)
				return;
		}
	}
}

/* Lists a port's BARs from the largest to the smallest, keeping the found order among equals. */
static void
order_bars(
	const struct drv_pci_vmd_port *port,
	unsigned *order)
{
	unsigned index;
	unsigned place;

	/* Inserts each BAR after the ones at least as large. */
	for (index = 0; index < port->bar_count; index++) {
		/* Moves the smaller BARs listed so far one place down. */
		place = index;
		while (place > 0 && port->bars[order[place - 1U]].size < port->bars[index].size) {
			order[place] = order[place - 1U];
			place--;
		}

		order[place] = index;
	}
}

/* Reports the space one BAR takes in a window: its size, and never less than a page. */
static uint64_t
bar_span(
	const struct drv_pci_vmd_bar *bar)
{
	/* A BAR smaller than a page still takes one. */
	if (bar->size < VMD_BAR_MIN_SPAN)
		return VMD_BAR_MIN_SPAN;

	/* Reports the BAR's own size. */
	return bar->size;
}

/*
 * Places a port's window in the VMD's windows and its BARs in the window.
 *
 * The window is a power of two of at least 1 MiB below 4 GiB, because a
 * bridge's memory window has 32-bit addresses; every BAR goes in it,
 * prefetchable or not, the largest first.  A port whose window fits
 * nowhere is marked so and left alone.
 */
static void
place_port(
	const struct drv_pci_vmd_domain *domain,
	struct drv_pci_vmd_plan *plan,
	struct drv_pci_vmd_port *port)
{
	struct drv_pci_range busy[DRV_PCI_VMD_PORTS_MAX];
	struct drv_pci_range placed[DRV_PCI_VMD_PORT_BARS_MAX];
	struct drv_pci_range window;
	unsigned order[DRV_PCI_VMD_PORT_BARS_MAX];
	struct drv_pci_vmd_bar *bar;
	uint64_t total;
	uint64_t span;
	uint64_t length;
	uint64_t address;
	unsigned busy_count;
	unsigned index;
	unsigned which;
	int error;

	/* A port with nothing behind it needs no window. */
	if (port->bar_count == 0)
		return;

	/* Adds up the space of the BARs laid end to end, the largest first. */
	order_bars(port, order);
	total = 0;
	for (index = 0; index < port->bar_count; index++) {
		/* Each BAR starts at a multiple of its own span. */
		span = bar_span(&port->bars[order[index]]);
		total = (total + (span - 1U)) & ~(span - 1U);
		total += span;
	}

	/* Rounds the total up to a power of two of at least the bridge granule. */
	length = VMD_WINDOW_GRANULE;
	while (length < total)
		length <<= 1;

	/* Lists the windows of the ports placed before this one. */
	busy_count = 0;
	for (index = 0; index < plan->port_count; index++) {
		/* Stops at this port; a port without a window takes no room. */
		if (&plan->ports[index] == port)
			break;
		if (plan->ports[index].window_length == 0)
			continue;

		busy[busy_count].base = plan->ports[index].window_base;
		busy[busy_count].length = plan->ports[index].window_length;
		busy_count++;
	}

	/* Finds a place for the window below 4 GiB in the VMD's windows. */
	error = drv_pci_window_place(domain->windows, domain->window_count, busy, busy_count, length, false, &address, &which);
	if (error != 0) {
		port->state = DRV_PCI_VMD_PORT_NO_WINDOW;
		return;
	}

	port->window_base = address;
	port->window_length = length;
	window.base = address;
	window.length = length;

	/* Places each BAR in the window, the largest first. */
	for (index = 0; index < port->bar_count; index++) {
		bar = &port->bars[order[index]];

		/* Finds the lowest free place aligned to the BAR's size. */
		error = drv_pci_window_place(&window, 1U, placed, index, bar_span(bar), false, &address, &which);
		if (error != 0) {
			port->state = DRV_PCI_VMD_PORT_NO_WINDOW;
			port->window_base = 0;
			port->window_length = 0;
			return;
		}

		bar->address = address;
		placed[index].base = address;
		placed[index].length = bar_span(bar);
	}
}

/* Writes the BARs, the window and the enables of a port in use. */
static void
write_port(
	const struct drv_pci_vmd_config_access *access,
	unsigned bus,
	struct drv_pci_vmd_port *port)
{
	uint64_t last;
	uint32_t window;
	uint32_t command;
	unsigned index;
	int error;

	/* Writes each BAR behind the port. */
	for (index = 0; index < port->bar_count; index++)
		write_bar(access, port->secondary, &port->bars[index], port->bars[index].address);

	/* A port with nothing behind it keeps its windows closed and its enables off. */
	if (port->window_length == 0)
		return;

	/* Opens the memory window: address bits 31:20 of its first and last bytes. */
	last = port->window_base + (port->window_length - 1U);
	window = (uint32_t)((port->window_base >> VMD_WINDOW_FIELD_SHIFT) & VMD_WINDOW_FIELD_MASK);
	window |= (uint32_t)((last >> VMD_WINDOW_FIELD_SHIFT) & VMD_WINDOW_FIELD_MASK) << VMD_WINDOW_LIMIT_SHIFT;
	(void)config_write(access, bus, port->device, port->function, VMD_BRIDGE_MEMORY_WINDOW, 4U, window);

	/* Lets the port decode its window and forward its children's requests. */
	error = config_read(access, bus, port->device, port->function, VMD_CFG_COMMAND, 2U, &command);
	if (error != 0)
		return;
	command |= VMD_CFG_COMMAND_MEMORY | VMD_CFG_COMMAND_MASTER;
	(void)config_write(access, bus, port->device, port->function, VMD_CFG_COMMAND, 2U, command & 0xffffU);
}

/*
 * Switches off a port that cannot be used, and what was measured behind it.
 *
 * Its bus numbers go to zero so that the PCI core makes no bus behind it;
 * its windows stay closed and its command register cleared.
 */
static void
retire_port(
	const struct drv_pci_vmd_config_access *access,
	unsigned bus,
	struct drv_pci_vmd_port *port)
{
	unsigned index;

	/* Clears the BARs the sizing left all ones, while the bus behind is still numbered. */
	if (port->secondary != 0) {
		for (index = 0; index < port->bar_count; index++)
			write_bar(access, port->secondary, &port->bars[index], 0);
	}

	/* Removes the port's bus numbers. */
	write_port_buses(access, bus, port, 0, 0);
	port->secondary = 0;
	port->window_base = 0;
	port->window_length = 0;
}

/* Writes one memory BAR, both halves of a 64-bit one. */
static void
write_bar(
	const struct drv_pci_vmd_config_access *access,
	unsigned bus,
	const struct drv_pci_vmd_bar *bar,
	uint64_t address)
{
	unsigned offset;

	offset = VMD_CFG_BAR0 + bar->index * 4U;

	/* Writes the low half; the type bits there are read-only. */
	(void)config_write(access, bus, bar->device, bar->function, offset, 4U, (uint32_t)address);

	/* Writes the upper half of a 64-bit BAR. */
	if (bar->wide)
		(void)config_write(access, bus, bar->device, bar->function, offset + 4U, 4U, (uint32_t)(address >> 32));
}
