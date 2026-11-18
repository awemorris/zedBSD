/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The receiving side of an ARM PrimeCell UART (PL011) behind a GIC.
 *
 * The device tree says where the registers are and which GIC interrupt
 * the UART raises.  The line's speed, framing and enables are the
 * hardware layer's, which writes the console through the same UART: the
 * driver writes only the interrupt mask and the interrupt clear register.
 *
 * Reading is by interrupt: a received character and the receive timeout
 * (a character that waits in the FIFO below its trigger level) both raise
 * it.  The handler clears what it is about to serve, then drains what the
 * receiver holds, a bounded number of bytes per interrupt, and hands each
 * byte to the receiver function its starter gave.  A byte that arrives
 * after the drain raises the interrupt again.
 *
 * Values referenced: ARM DDI 0183G, PrimeCell UART (PL011) Technical
 * Reference Manual (UARTDR 0x000, UARTFR 0x018 and its RXFE bit 4,
 * UARTIMSC 0x038, UARTICR 0x044; the receive interrupt bit 4 and the
 * receive timeout bit 5 of the mask and clear registers, and the eleven
 * interrupt bits 0 to 10); the binding "arm,pl011" and the GIC's
 * three-cell interrupt specifier (type, number, flags; type 0 a shared
 * peripheral interrupt numbered from 32, type 1 a private one numbered
 * from 16) of the Linux v6.12 device tree bindings
 * (serial/pl011.yaml, interrupt-controller/arm,gic.yaml).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <drivers/serial/pl011.h>

#include <drivers/generic/fdt.h>
#include <kern/device-io.h>
#include <kern/irq.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

/* The registers the driver uses, by their offsets. */
#define PL011_DATA		0x000U
#define PL011_FLAGS		0x018U
#define PL011_INTERRUPT_MASK	0x038U
#define PL011_INTERRUPT_CLEAR	0x044U

/* The flag register's bit that says the receiver holds nothing. */
#define PL011_FLAGS_RX_EMPTY	0x010U

/* The receive and receive-timeout interrupts, and every interrupt the UART has. */
#define PL011_INTERRUPT_RX	0x010U
#define PL011_INTERRUPT_RX_TIMEOUT 0x020U
#define PL011_INTERRUPT_ALL	0x7ffU

/* The received character in the data register, below its error bits. */
#define PL011_DATA_CHARACTER	0x0ffU

/* The GIC's interrupt specifier: its three cells, and where each type's numbers start. */
#define PL011_GIC_CELLS		3U
#define PL011_GIC_TYPE_SHARED	0U
#define PL011_GIC_TYPE_PRIVATE	1U
#define PL011_GIC_FIRST_SHARED	32U
#define PL011_GIC_FIRST_PRIVATE	16U

/*
 * How many bytes one interrupt takes.  A line that never stops delivering
 * must not keep the processor in the handler for ever; what is left raises
 * the interrupt again.
 */
#define PL011_DRAIN_LIMIT	64U

/* How much address space the registers take when the tree gives no size. */
#define PL011_DEFAULT_WINDOW	0x1000U

/* Marks a parameter the interrupt handler's signature carries but the driver does not need. */
#define UNUSED_PARAMETER(parameter)	((void)(parameter))

static int find_uart(const struct drv_fdt *fdt, uint32_t *node);
static int read_interrupt(const struct drv_fdt *fdt, uint32_t node, int *irq);
static uint32_t read_register(const struct drv_pl011 *port, uint32_t offset);
static void write_register(const struct drv_pl011 *port, uint32_t offset, uint32_t value);
static void drain_receiver(struct drv_pl011 *port, bool deliver);
static void uart_interrupt(int irq, kern_irq_ack_t acknowledge, void *argument);

/*
 * Finds the first present PL011 the device tree describes and maps it.
 */
int
drv_pl011_probe(
	const struct drv_fdt *fdt,
	struct drv_pl011 *port)
{
	uint32_t node;
	uint64_t physical;
	uint64_t window;
	void *mapped;
	int irq;
	int error;

	/* Refuses a missing tree or result. */
	if (fdt == NULL || port == NULL)
		return EINVAL;

	/* Finds the UART's node. */
	error = find_uart(fdt, &node);
	if (error != 0)
		return error;

	/* Reads where the registers are; a missing size takes the whole window. */
	error = drv_fdt_reg(fdt, node, 0, &physical, &window);
	if (error != 0)
		return error;

	/* A tree that gives no size gets the whole window. */
	if (window == 0)
		window = PL011_DEFAULT_WINDOW;

	/* Reads the GIC interrupt the UART raises. */
	error = read_interrupt(fdt, node, &irq);
	if (error != 0)
		return error;

	/* Maps the registers, uncached as every device register is. */
	error = kern_device_map(physical, (size_t)window, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0)
		return error;

	/* Fills the port; input is started separately. */
	port->physical = physical;
	port->base = (uintptr_t)mapped;
	port->irq = irq;
	port->receive = NULL;
	port->input_started = false;

	/* Succeeded: the UART's registers are reachable. */
	return 0;
}

/*
 * Starts taking received bytes by interrupt.
 */
int
drv_pl011_start_input(
	struct drv_pl011 *port,
	drv_pl011_receive_t receive)
{
	uint32_t mask;
	int error;

	/* Refuses a missing port or receiver, and a second start. */
	if (port == NULL || receive == NULL)
		return EINVAL;
	if (port->input_started)
		return EBUSY;

	/* Installs the handler; the interrupt stays masked until the end. */
	error = kern_irq_register(port->irq, uart_interrupt, port);
	if (error != 0)
		return error;

	/*
	 * Drops what the line carried before anything could read it, and
	 * whatever interrupt it left raised: the first byte the reader sees
	 * must be the first one typed to it.
	 */
	drain_receiver(port, false);
	write_register(port, PL011_INTERRUPT_CLEAR, PL011_INTERRUPT_ALL);

	/*
	 * Publishes the receiver before the interrupt can run: the handler
	 * delivers only once input_started is set.
	 */
	port->receive = receive;
	port->input_started = true;

	/* Has the UART raise its interrupt for a received character and for one left waiting. */
	mask = read_register(port, PL011_INTERRUPT_MASK);
	mask |= PL011_INTERRUPT_RX | PL011_INTERRUPT_RX_TIMEOUT;
	write_register(port, PL011_INTERRUPT_MASK, mask);

	/* Lets the interrupt through to the handler. */
	kern_irq_unmask(port->irq);

	/* Succeeded: received bytes go to the receiver. */
	return 0;
}

/* Finds the first present node of a PL011. */
static int
find_uart(
	const struct drv_fdt *fdt,
	uint32_t *node)
{
	uint32_t candidate;
	bool present;
	int error;

	/* Walks the PL011 nodes until a present one is found. */
	candidate = DRV_FDT_NO_NODE;
	for (;;) {
		/* Finds the next node; none left means the tree has no usable UART. */
		error = drv_fdt_find_compatible(fdt, "arm,pl011", candidate, &candidate);
		if (error != 0)
			return error;

		/* Skips a node the tree marks as not present. */
		present = drv_fdt_node_enabled(fdt, candidate);
		if (present)
			break;
	}

	/* Succeeded: the first present PL011. */
	*node = candidate;
	return 0;
}

/* Reads the GIC interrupt number of a node's first interrupt specifier. */
static int
read_interrupt(
	const struct drv_fdt *fdt,
	uint32_t node,
	int *irq)
{
	const uint8_t *value;
	uint32_t length;
	uint32_t type;
	uint32_t number;
	int error;

	/* A UART without an interrupt cannot be read by interrupt. */
	error = drv_fdt_property(fdt, node, "interrupts", &value, &length);
	if (error != 0)
		return error;

	/* Refuses a specifier that is not the GIC's three cells. */
	if (length < PL011_GIC_CELLS * 4U)
		return EINVAL;

	/* The specifier's type and its number within that type. */
	type = (uint32_t)drv_fdt_cells_value(value, 0, 1);
	number = (uint32_t)drv_fdt_cells_value(value, 1, 1);

	/* Numbers the interrupt as the GIC does. */
	if (type == PL011_GIC_TYPE_SHARED) {
		*irq = (int)(number + PL011_GIC_FIRST_SHARED);
	} else if (type == PL011_GIC_TYPE_PRIVATE) {
		*irq = (int)(number + PL011_GIC_FIRST_PRIVATE);
	} else {
		return EINVAL;
	}

	/* Succeeded: the interrupt's GIC number. */
	return 0;
}

/* Reads one 32-bit register. */
static uint32_t
read_register(
	const struct drv_pl011 *port,
	uint32_t offset)
{
	uint32_t value;

	/* Every PL011 register is read as a word. */
	value = kern_mmio_read32((const volatile void *)(port->base + offset));

	/* Succeeded: the register's value. */
	return value;
}

/* Writes one 32-bit register. */
static void
write_register(
	const struct drv_pl011 *port,
	uint32_t offset,
	uint32_t value)
{
	/* Every PL011 register is written as a word. */
	kern_mmio_write32((volatile void *)(port->base + offset), value);
}

/*
 * Takes what the receiver holds, at most one interrupt's worth, and hands
 * each byte to the receiver when deliver is set.
 */
static void
drain_receiver(
	struct drv_pl011 *port,
	bool deliver)
{
	unsigned drained;
	uint32_t flags;
	uint32_t data;

	/* Takes each byte the receiver holds. */
	for (drained = 0; drained < PL011_DRAIN_LIMIT; drained++) {
		/* Stops once the receiver holds nothing. */
		flags = read_register(port, PL011_FLAGS);
		if ((flags & PL011_FLAGS_RX_EMPTY) != 0)
			return;

		/* Takes the byte, which also frees its place in the receiver. */
		data = read_register(port, PL011_DATA);

		/* Hands the byte on, unless it is one from before the start. */
		if (deliver)
			port->receive((uint8_t)(data & PL011_DATA_CHARACTER));
	}
}

/*
 * Serves the UART's interrupt: clears the receive interrupts, hands the
 * received bytes on and ends the interrupt.
 */
static void
uart_interrupt(
	int irq,
	kern_irq_ack_t acknowledge,
	void *argument)
{
	struct drv_pl011 *port;

	UNUSED_PARAMETER(irq);

	/* Finds the port the handler was installed for. */
	port = (struct drv_pl011 *)argument;

	/* Clears what is served now; a byte that arrives after the drain raises the interrupt again. */
	write_register(port, PL011_INTERRUPT_CLEAR, PL011_INTERRUPT_ALL);

	/* Hands what arrived to the receiver. */
	drain_receiver(port, port->input_started);

	/* Ends the interrupt at its controller. */
	kern_irq_send_eoi(acknowledge);
}
