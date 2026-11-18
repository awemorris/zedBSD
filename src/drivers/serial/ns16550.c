/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A memory-mapped 16550-compatible UART.
 *
 * The device tree says where the registers are, how far apart they lie
 * (reg-shift) and how wide an access must be (reg-io-width), and which
 * interrupt the UART raises.  The line's speed and framing are the
 * firmware's: neither the divisor nor the line control register is written,
 * so the console the firmware set up keeps working.
 *
 * Reading is by interrupt.  The handler drains what the receiver holds, a
 * bounded number of bytes per interrupt, and hands each byte to the
 * receiver function its starter gave.
 *
 * Sending is by polling, for a kernel that writes its console's characters
 * to the line itself: each byte waits a bounded time for the transmitter,
 * with interrupts off and the other processors kept out, and is dropped if
 * the transmitter never frees.  It never sleeps, so the kernel's character
 * path may use it from an interrupt and on a fatal stop.
 *
 * The UART the firmware's console uses is the one the device tree's
 * /chosen/stdout-path names, either by its path or by an alias the
 * /aliases node resolves to a path.
 *
 * The Synopsys DesignWare UART is a 16550 with one more interrupt: a write
 * to the line control register while the UART is busy is ignored and raises
 * a "busy detect" interrupt, which stays raised until the UART status
 * register is read.  Nothing here writes that register, but the firmware
 * may have, so the handler clears the condition when it finds it.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <drivers/serial/ns16550.h>

#include <drivers/generic/fdt.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/irq.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

/*
 * The registers, as register numbers before the device tree's reg-shift
 * spaces them out, and the bits of them the driver uses.
 *
 * Values referenced: the 16550 UART register map as the National
 * Semiconductor PC16550D data sheet defines it (receiver buffer and
 * transmitter holding 0, interrupt enable 1, interrupt identification 2,
 * line status 5; the received-data-available enable bit, the interrupt
 * identity field, the data-ready and transmitter-holding-register-empty
 * status bits).
 */
#define NS16550_RECEIVER_BUFFER		0U
#define NS16550_TRANSMITTER_HOLDING	0U
#define NS16550_INTERRUPT_ENABLE	1U
#define NS16550_INTERRUPT_IDENTITY	2U
#define NS16550_LINE_STATUS		5U
#define NS16550_ENABLE_RECEIVED_DATA	0x01U
#define NS16550_IDENTITY_MASK		0x0fU
#define NS16550_STATUS_DATA_READY	0x01U
#define NS16550_STATUS_TRANSMIT_EMPTY	0x20U

/*
 * The DesignWare UART's status register and the interrupt identity of its
 * busy-detect condition.
 *
 * Values referenced: Linux 6.8.12 drivers/tty/serial/8250/8250_dw.c (the
 * UART status register's register number and the busy-detect value of the
 * interrupt identification register).
 */
#define DW_UART_STATUS			0x1fU
#define DW_UART_IDENTITY_BUSY		0x07U

/*
 * How many bytes one interrupt takes.  A line that never stops delivering
 * must not keep the processor in the handler for ever; what is left raises
 * the interrupt again.
 */
#define NS16550_DRAIN_LIMIT		64U

/*
 * How much address space the registers are mapped with when the tree gives
 * no size: the sixteen-register window at the widest spacing.
 */
#define NS16550_DEFAULT_WINDOW		0x100U

/*
 * How many times a sender reads the line status for a free transmitter
 * before it drops the byte, and how many times it tries for the line
 * before it sends without it (a holder that stopped on a fatal error never
 * lets go).
 */
#define NS16550_TRANSMIT_WAIT_LIMIT	100000U
#define NS16550_LOCK_SPIN_LIMIT		1000000U

/*
 * The tokens of a flattened device tree's structure block that a path
 * walk meets before the root node, and the longest alias name looked up.
 *
 * Values referenced: Devicetree Specification v0.4, section 5.4.1
 * (FDT_BEGIN_NODE 0x1, FDT_NOP 0x4).
 */
#define NS16550_FDT_BEGIN_NODE		0x00000001U
#define NS16550_FDT_NOP			0x00000004U
#define NS16550_ALIAS_MAX		32U

/* Marks a parameter the interrupt handler's signature carries but the driver does not need. */
#define UNUSED_PARAMETER(parameter)	((void)(parameter))

/*
 * The device-tree compatible strings of the UARTs the driver takes, in the
 * order they are looked for.  The second field says whether the UART is a
 * DesignWare one with the busy-detect interrupt.
 */
struct ns16550_compatible {
	const char *name;
	bool busy_detect;
};

/*
 * The UARTs the driver recognizes, fixed at build time.
 */
static const struct ns16550_compatible ns16550_compatibles[] = {
	{"ns16550a", false},
	{"ns16550", false},
	{"snps,dw-apb-uart", true}
};

static int find_uart(const struct drv_fdt *fdt, uint32_t *node, bool *busy_detect);
static int map_uart(const struct drv_fdt *fdt, uint32_t node, bool busy_detect, bool need_interrupt, struct drv_ns16550 *port);
static int console_node(const struct drv_fdt *fdt, uint32_t *node);
static int node_kind(const struct drv_fdt *fdt, uint32_t node, bool *busy_detect);
static int find_path(const struct drv_fdt *fdt, const char *path, uint32_t length, uint32_t *node);
static int find_child(const struct drv_fdt *fdt, uint32_t parent, const char *component, uint32_t length, uint32_t *child);
static int root_node(const struct drv_fdt *fdt, uint32_t *node);
static int node_name(const struct drv_fdt *fdt, uint32_t node, const char **name, uint32_t *length);
static int string_property(const struct drv_fdt *fdt, uint32_t node, const char *name, const char **string, uint32_t *length);
static uint32_t blob_be32(const uint8_t *bytes);
static uint32_t read_cell_property(const struct drv_fdt *fdt, uint32_t node, const char *name, uint32_t fallback, int *error);
static uint32_t read_register(const struct drv_ns16550 *port, unsigned number);
static void write_register(const struct drv_ns16550 *port, unsigned number, uint32_t value);
static void drain_receiver(struct drv_ns16550 *port, bool deliver);
static void uart_interrupt(int irq, kern_irq_ack_t acknowledge, void *argument);

/*
 * Finds the first present UART the device tree describes and maps it.
 */
int
drv_ns16550_probe(
	const struct drv_fdt *fdt,
	struct drv_ns16550 *port)
{
	uint32_t node;
	bool busy_detect;
	int error;

	/* Refuses a missing tree or result. */
	if (fdt == NULL || port == NULL)
		return EINVAL;

	/* Finds the UART's node and whether it is a DesignWare one. */
	error = find_uart(fdt, &node, &busy_detect);
	if (error != 0)
		return error;

	/* Maps it; a UART read by interrupt needs one. */
	error = map_uart(fdt, node, busy_detect, true, port);
	if (error != 0)
		return error;

	/* Succeeded: the UART's registers are reachable. */
	return 0;
}

/*
 * Finds the UART the device tree's /chosen/stdout-path names and maps it.
 */
int
drv_ns16550_probe_console(
	const struct drv_fdt *fdt,
	struct drv_ns16550 *port)
{
	uint32_t node;
	bool busy_detect;
	bool present;
	int error;

	/* Refuses a missing tree or result. */
	if (fdt == NULL || port == NULL)
		return EINVAL;

	/* Finds the node the console names. */
	error = console_node(fdt, &node);
	if (error != 0)
		return error;

	/* A console the tree marks as not present is not used. */
	present = drv_fdt_node_enabled(fdt, node);
	if (!present)
		return ENOENT;

	/* Refuses a node that is not a UART the driver knows. */
	error = node_kind(fdt, node, &busy_detect);
	if (error != 0)
		return error;

	/* Maps it; a console only sent to needs no interrupt. */
	error = map_uart(fdt, node, busy_detect, false, port);
	if (error != 0)
		return error;

	/* Succeeded: the console's registers are reachable. */
	return 0;
}

/*
 * Starts taking received bytes by interrupt.
 */
int
drv_ns16550_start_input(
	struct drv_ns16550 *port,
	drv_ns16550_receive_t receive)
{
	uint32_t enable;
	int error;

	/* Refuses a missing port or receiver, and a second start. */
	if (port == NULL || receive == NULL)
		return EINVAL;
	if (port->input_started)
		return EBUSY;

	/* A UART without an interrupt cannot be read by interrupt. */
	if (port->irq == 0)
		return ENOENT;

	/* Installs the handler; the interrupt stays masked until the end. */
	error = kern_irq_register(port->irq, uart_interrupt, port);
	if (error != 0)
		return error;

	/*
	 * Drops what the line carried before anything could read it: a line
	 * connected since power-on may have carried anything, and the first
	 * byte the reader sees must be the first one typed to it.
	 */
	drain_receiver(port, false);

	/*
	 * Publishes the receiver before the interrupt can run: the handler
	 * delivers only once input_started is set.
	 */
	port->receive = receive;
	port->input_started = true;

	/* Has the UART raise its interrupt when a byte arrives. */
	enable = read_register(port, NS16550_INTERRUPT_ENABLE);
	enable |= NS16550_ENABLE_RECEIVED_DATA;
	write_register(port, NS16550_INTERRUPT_ENABLE, enable);

	/* Lets the interrupt through to the handler. */
	kern_irq_unmask(port->irq);

	/* Succeeded: received bytes go to the receiver. */
	return 0;
}

/*
 * Sends one byte once the transmitter is free, or drops it.
 */
void
drv_ns16550_putc_polled(
	struct drv_ns16550 *port,
	uint8_t byte)
{
	bool interrupts_enabled;
	unsigned previous;
	unsigned spins;
	unsigned waited;
	uint32_t status;
	bool owned;

	/* Ignores a missing port. */
	if (port == NULL)
		return;

	/* Keeps an interrupt's byte from coming between this one's wait and its write. */
	interrupts_enabled = kern_irq_disable();

	/*
	 * Takes the line from the other processors.  A holder that never lets
	 * go (one stopped on a fatal error, or this processor interrupted by
	 * a fatal stop inside the send) is waited for a bounded time only,
	 * and the byte then goes out without the line.
	 */
	owned = false;
	for (spins = 0; spins < NS16550_LOCK_SPIN_LIMIT; spins++) {
		/* Stops once this processor holds the line. */
		previous = __atomic_exchange_n(&port->output_lock, 1U, __ATOMIC_ACQUIRE);
		if (previous == 0U) {
			owned = true;
			break;
		}
	}

	/* Waits a bounded time for the transmitter's holding register. */
	for (waited = 0; waited < NS16550_TRANSMIT_WAIT_LIMIT; waited++) {
		/* Stops once the transmitter can take a byte. */
		status = read_register(port, NS16550_LINE_STATUS);
		if ((status & NS16550_STATUS_TRANSMIT_EMPTY) != 0)
			break;
	}

	/* Sends the byte, unless the transmitter never became free. */
	if (waited < NS16550_TRANSMIT_WAIT_LIMIT)
		write_register(port, NS16550_TRANSMITTER_HOLDING, byte);

	/* Lets the other processors send. */
	if (owned)
		__atomic_store_n(&port->output_lock, 0U, __ATOMIC_RELEASE);

	/* Restores interrupts only when they were on. */
	if (interrupts_enabled)
		kern_irq_enable();
}

/*
 * Maps one UART's registers from its node and fills the port.  A UART read
 * by interrupt needs one (need_interrupt); without it irq is 0.
 */
static int
map_uart(
	const struct drv_fdt *fdt,
	uint32_t node,
	bool busy_detect,
	bool need_interrupt,
	struct drv_ns16550 *port)
{
	uint64_t physical;
	uint64_t window;
	uint32_t shift;
	uint32_t width;
	uint32_t irq;
	void *mapped;
	int error;

	/* Reads where the registers are; a missing size takes the whole window. */
	error = drv_fdt_reg(fdt, node, 0, &physical, &window);
	if (error != 0)
		return error;

	/* A tree that gives no size gets the whole window. */
	if (window == 0)
		window = NS16550_DEFAULT_WINDOW;

	/* Reads how far apart the registers are; one byte apart when unstated. */
	shift = read_cell_property(fdt, node, "reg-shift", 0, &error);
	if (error != 0)
		return error;

	/* Refuses a spacing wider than a 32-bit register's. */
	if (shift > 4)
		return EINVAL;

	/* Reads how wide an access is; a byte when unstated. */
	width = read_cell_property(fdt, node, "reg-io-width", 1, &error);
	if (error != 0)
		return error;

	/* Refuses a width the driver cannot access with. */
	if (width != 1 && width != 4)
		return EINVAL;

	/* Reads the interrupt the UART raises at its controller. */
	irq = read_cell_property(fdt, node, "interrupts", 0, &error);
	if (error != 0)
		return error;

	/* A UART read by interrupt needs one. */
	if (need_interrupt && irq == 0)
		return ENOENT;

	/* Maps the registers, uncached as every device register is. */
	error = kern_device_map(physical, (size_t)window, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0)
		return error;

	/* Fills the port; input is started separately. */
	port->physical = physical;
	port->base = (uintptr_t)mapped;
	port->reg_shift = shift;
	port->io_width = width;
	port->irq = (int)irq;
	port->busy_detect = busy_detect;
	port->receive = NULL;
	port->input_started = false;
	port->output_lock = 0U;

	/* Succeeded: the UART's registers are reachable. */
	return 0;
}

/* Finds the first present node of a recognized UART. */
static int
find_uart(
	const struct drv_fdt *fdt,
	uint32_t *node,
	bool *busy_detect)
{
	uint32_t candidate;
	size_t index;
	bool present;
	bool found;
	int error;

	/* Looks for each recognized kind in turn. */
	found = false;
	for (index = 0;
	     index < sizeof(ns16550_compatibles) / sizeof(ns16550_compatibles[0]);
	     index++) {
		/* Walks the nodes of this kind until a present one is found. */
		candidate = DRV_FDT_NO_NODE;
		for (;;) {
			/* Finds the next node of this kind; none left moves to the next kind. */
			error = drv_fdt_find_compatible(fdt, ns16550_compatibles[index].name, candidate, &candidate);
			if (error == ENOENT)
				break;

			/* Reports a tree the search could not read. */
			if (error != 0)
				return error;

			/* Skips a node the tree marks as not present. */
			present = drv_fdt_node_enabled(fdt, candidate);
			if (!present)
				continue;

			/* Takes the first present UART of this kind. */
			found = true;
			break;
		}

		/* Stops at the first kind that has a present UART. */
		if (found)
			break;
	}

	/* The tree describes no present UART the driver knows. */
	if (!found)
		return ENOENT;

	/* Reports the node and whether it is a DesignWare UART. */
	*node = candidate;
	*busy_detect = ns16550_compatibles[index].busy_detect;

	/* Succeeded: the first present UART the driver knows. */
	return 0;
}

/* Reads the first cell of a property, or the fallback when it is absent. */
static uint32_t
read_cell_property(
	const struct drv_fdt *fdt,
	uint32_t node,
	const char *name,
	uint32_t fallback,
	int *error)
{
	const uint8_t *value;
	uint32_t length;
	uint32_t number;
	int lookup;

	/* Looks the property up. */
	*error = 0;
	lookup = drv_fdt_property(fdt, node, name, &value, &length);

	/* An absent property takes the fallback. */
	if (lookup == ENOENT)
		return fallback;

	/* Reports a tree the lookup could not read. */
	if (lookup != 0) {
		*error = lookup;
		return 0;
	}

	/* Refuses a value shorter than one cell. */
	if (length < 4) {
		*error = EINVAL;
		return 0;
	}

	/* Reads the first cell. */
	number = (uint32_t)drv_fdt_cells_value(value, 0, 1);

	/* Succeeded: the property's first cell. */
	return number;
}

/* Reads one register at its spacing and with the access width the UART needs. */
static uint32_t
read_register(
	const struct drv_ns16550 *port,
	unsigned number)
{
	uintptr_t address;
	uint32_t value;

	/* Places the register by the tree's spacing. */
	address = port->base + ((uintptr_t)number << port->reg_shift);

	/* Reads it as wide as the UART requires. */
	if (port->io_width == 4) {
		value = kern_mmio_read32((const volatile void *)address);
	} else {
		value = kern_mmio_read8((const volatile void *)address);
	}

	/* Succeeded: the register's value. */
	return value;
}

/* Writes one register at its spacing and with the access width the UART needs. */
static void
write_register(
	const struct drv_ns16550 *port,
	unsigned number,
	uint32_t value)
{
	uintptr_t address;

	/* Places the register by the tree's spacing. */
	address = port->base + ((uintptr_t)number << port->reg_shift);

	/* Writes it as wide as the UART requires. */
	if (port->io_width == 4) {
		kern_mmio_write32((volatile void *)address, value);
	} else {
		kern_mmio_write8((volatile void *)address, (uint8_t)value);
	}
}

/*
 * Takes what the receiver holds, at most one interrupt's worth, and hands
 * each byte to the receiver when deliver is set.
 */
static void
drain_receiver(
	struct drv_ns16550 *port,
	bool deliver)
{
	unsigned drained;
	uint32_t status;
	uint8_t byte;

	/* Takes each byte the receiver holds. */
	for (drained = 0; drained < NS16550_DRAIN_LIMIT; drained++) {
		/* Stops once the receiver holds nothing. */
		status = read_register(port, NS16550_LINE_STATUS);
		if ((status & NS16550_STATUS_DATA_READY) == 0)
			return;

		/* Takes the byte, which also frees its place in the receiver. */
		byte = (uint8_t)read_register(port, NS16550_RECEIVER_BUFFER);

		/* Hands the byte on, unless it is one from before the start. */
		if (deliver)
			port->receive(byte);
	}
}

/*
 * Serves the UART's interrupt: clears a DesignWare busy-detect condition,
 * hands the received bytes on and ends the interrupt.
 */
static void
uart_interrupt(
	int irq,
	kern_irq_ack_t acknowledge,
	void *argument)
{
	struct drv_ns16550 *port;
	uint32_t raw_identity;
	uint32_t identity;

	UNUSED_PARAMETER(irq);

	/* Finds the port the handler was installed for. */
	port = (struct drv_ns16550 *)argument;

	/*
	 * Reading the status register is what ends a busy-detect interrupt;
	 * left alone, it would be raised again for ever.
	 */
	raw_identity = read_register(port, NS16550_INTERRUPT_IDENTITY);
	identity = raw_identity & NS16550_IDENTITY_MASK;
	if (port->busy_detect && identity == DW_UART_IDENTITY_BUSY)
		(void)read_register(port, DW_UART_STATUS);

	/* Hands what arrived to the receiver. */
	drain_receiver(port, port->input_started);

	/* Ends the interrupt at its controller. */
	kern_irq_send_eoi(acknowledge);
}

/*
 * Finds the node /chosen/stdout-path names: a path, or an alias that the
 * /aliases node resolves to one; the line's options after a colon are not
 * part of the name.
 */
static int
console_node(
	const struct drv_fdt *fdt,
	uint32_t *node)
{
	char alias[NS16550_ALIAS_MAX];
	const char *colon;
	const char *path;
	uint32_t aliases;
	uint32_t chosen;
	uint32_t length;
	int error;

	/* Finds the node that carries the firmware's choices. */
	error = find_path(fdt, "/chosen", 7U, &chosen);
	if (error != 0)
		return error;

	/* Reads the console's name, the standard property first. */
	error = string_property(fdt, chosen, "stdout-path", &path, &length);
	if (error == ENOENT)
		error = string_property(fdt, chosen, "linux,stdout-path", &path, &length);
	if (error != 0)
		return error;

	/* Drops the line's options after a colon. */
	colon = kern_memchr(path, ':', length);
	if (colon != NULL)
		length = (uint32_t)(colon - path);

	/* A path names the node itself. */
	if (length != 0U && path[0] == '/') {
		error = find_path(fdt, path, length, node);
		if (error != 0)
			return error;

		/* Succeeded: the node the path names. */
		return 0;
	}

	/* An alias name too short or too long to be looked up has no entry. */
	if (length == 0U || length >= NS16550_ALIAS_MAX)
		return ENOENT;

	/* The alias as a terminated property name. */
	kern_memcpy(alias, path, length);
	alias[length] = '\0';

	/* Finds the node that resolves aliases. */
	error = find_path(fdt, "/aliases", 8U, &aliases);
	if (error != 0)
		return error;

	/* Reads the path the alias stands for, which must be absolute. */
	error = string_property(fdt, aliases, alias, &path, &length);
	if (error != 0)
		return error;
	if (length == 0U || path[0] != '/')
		return ENOENT;

	/* Finds the node at that path. */
	error = find_path(fdt, path, length, node);
	if (error != 0)
		return error;

	/* Succeeded: the node the alias names. */
	return 0;
}

/* Reports whether a node is a UART the driver knows, and whether it is a DesignWare one. */
static int
node_kind(
	const struct drv_fdt *fdt,
	uint32_t node,
	bool *busy_detect)
{
	uint32_t candidate;
	size_t index;
	int error;

	/* Looks for the node among the nodes of each recognized kind in turn. */
	for (index = 0;
	     index < sizeof(ns16550_compatibles) / sizeof(ns16550_compatibles[0]);
	     index++) {
		/* Walks the nodes of this kind. */
		candidate = DRV_FDT_NO_NODE;
		for (;;) {
			/* Finds the next node of this kind; none left moves to the next kind. */
			error = drv_fdt_find_compatible(fdt, ns16550_compatibles[index].name, candidate, &candidate);
			if (error == ENOENT)
				break;

			/* Reports a tree the search could not read. */
			if (error != 0)
				return error;

			/* The node is of this kind. */
			if (candidate == node) {
				*busy_detect = ns16550_compatibles[index].busy_detect;
				return 0;
			}
		}
	}

	/* The node is no UART the driver knows. */
	return ENOENT;
}

/* Finds the node at an absolute path of length bytes. */
static int
find_path(
	const struct drv_fdt *fdt,
	const char *path,
	uint32_t length,
	uint32_t *node)
{
	uint32_t current;
	uint32_t start;
	uint32_t end;
	int error;

	/* The walk starts at the root. */
	error = root_node(fdt, &current);
	if (error != 0)
		return error;

	/* Steps down one component at a time. */
	start = 0;
	while (start < length) {
		/* Skips the separators before a component; a path may end in one. */
		if (path[start] == '/') {
			start++;
			continue;
		}

		/* The component runs to the next separator or the end. */
		end = start;
		while (end < length && path[end] != '/')
			end++;

		/* Finds the child of that name. */
		error = find_child(fdt, current, path + start, end - start, &current);
		if (error != 0)
			return error;
		start = end;
	}

	/* Succeeded: the node at the path. */
	*node = current;
	return 0;
}

/*
 * Finds a node's child by one path component of length bytes: its whole
 * name, or, for a component without a unit address, the name before the
 * at sign.
 */
static int
find_child(
	const struct drv_fdt *fdt,
	uint32_t parent,
	const char *component,
	uint32_t length,
	uint32_t *child)
{
	const char *name;
	const void *unit;
	uint32_t name_length;
	uint32_t candidate;
	int compared;
	int error;

	/* Says whether the component carries a unit address of its own. */
	unit = kern_memchr(component, '@', length);

	/* Looks at each child in turn. */
	candidate = DRV_FDT_NO_NODE;
	for (;;) {
		/* Takes the next child; none left means no such child. */
		error = drv_fdt_child(fdt, parent, candidate, &candidate);
		if (error != 0)
			return error;

		/* Reads the child's name. */
		error = node_name(fdt, candidate, &name, &name_length);
		if (error != 0)
			return error;

		/* A child shorter than the component cannot match. */
		if (name_length < length)
			continue;

		/* The component's bytes begin the name. */
		compared = kern_memcmp(name, component, length);
		if (compared != 0)
			continue;

		/* The whole name matches. */
		if (name_length == length)
			break;

		/* A component without a unit address matches the name before its at sign. */
		if (unit == NULL && name[length] == '@')
			break;
	}

	/* Succeeded: the child the component names. */
	*child = candidate;
	return 0;
}

/* Finds the root node: the first node of the structure block, after any no-operation tokens. */
static int
root_node(
	const struct drv_fdt *fdt,
	uint32_t *node)
{
	uint32_t offset;
	uint32_t end;
	uint32_t token;

	/* Reads the tokens of the structure block from its start. */
	offset = fdt->structure_offset;
	end = fdt->structure_offset + fdt->structure_size;
	while (end - offset >= 4U) {
		/* Takes the token at the walk's position. */
		token = blob_be32(fdt->blob + offset);

		/* Passes over a no-operation token. */
		if (token == NS16550_FDT_NOP) {
			offset += 4U;
			continue;
		}

		/* The first node is the root. */
		if (token == NS16550_FDT_BEGIN_NODE) {
			*node = offset;
			return 0;
		}

		/* Anything else before the root is a broken tree. */
		return EINVAL;
	}

	/* A structure block without a node is a broken tree. */
	return EINVAL;
}

/* Reads a node's name, which follows its begin-node token, terminated inside the structure block. */
static int
node_name(
	const struct drv_fdt *fdt,
	uint32_t node,
	const char **name,
	uint32_t *length)
{
	uint32_t start;
	uint32_t end;
	size_t found;

	/* Refuses a node outside the structure block. */
	end = fdt->structure_offset + fdt->structure_size;
	if (node < fdt->structure_offset || node > end)
		return EINVAL;
	if (end - node < 4U)
		return EINVAL;

	/* The name starts after the token and must end before the block does. */
	start = node + 4U;
	found = kern_strnlen((const char *)fdt->blob + start, end - start);
	if (found >= end - start)
		return EINVAL;

	/* Succeeded: the node's name. */
	*name = (const char *)fdt->blob + start;
	*length = (uint32_t)found;
	return 0;
}

/* Reads a property holding a terminated string, reporting the string's length. */
static int
string_property(
	const struct drv_fdt *fdt,
	uint32_t node,
	const char *name,
	const char **string,
	uint32_t *length)
{
	const uint8_t *value;
	uint32_t size;
	size_t found;
	int error;

	/* Looks the property up; an absent one reports ENOENT. */
	error = drv_fdt_property(fdt, node, name, &value, &size);
	if (error != 0)
		return error;

	/* Refuses a value that is not a terminated string. */
	found = kern_strnlen((const char *)value, size);
	if (found >= size)
		return EINVAL;

	/* Succeeded: the string and its length. */
	*string = (const char *)value;
	*length = (uint32_t)found;
	return 0;
}

/* Reads one big-endian word of the blob. */
static uint32_t
blob_be32(
	const uint8_t *bytes)
{
	uint32_t value;

	/* The most significant byte comes first. */
	value = ((uint32_t)bytes[0] << 24) |
	    ((uint32_t)bytes[1] << 16) |
	    ((uint32_t)bytes[2] << 8) |
	    (uint32_t)bytes[3];

	/* Succeeded: the word's value. */
	return value;
}
