/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The receiving side of a Cadence UART, the Zynq-7000's.
 *
 * The board names the UART's address and GIC interrupt.  The line's speed,
 * framing and enables are the hardware layer's, which writes the console
 * through the same UART: the driver writes only the receive trigger level,
 * the receive timeout and the interrupt registers.
 *
 * Reading is by interrupt: the receive trigger (a byte in the FIFO, with
 * the trigger level at one) and the receive timeout both raise it.  The
 * handler clears the status it is about to serve, then drains what the
 * receiver holds, a bounded number of bytes per interrupt, and hands each
 * byte to the receiver function its starter gave.  A byte that arrives
 * after the drain raises the interrupt again.
 *
 * Values referenced: Linux v6.12 drivers/tty/serial/xilinx_uartps.c, the
 * register offsets CDNS_UART_IER 0x08, IDR 0x0c, ISR 0x14, RXTOUT 0x1c,
 * RXWM 0x20, SR 0x2c and FIFO 0x30, the interrupt bits
 * CDNS_UART_IXR_RXTRIG (bit 0) and CDNS_UART_IXR_TOUT (bit 8), the status
 * bit CDNS_UART_SR_RXEMPTY (bit 1), the default receive timeout of 10 and
 * the write of the read status back to the ISR to clear it.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <drivers/serial/cdns-uart.h>

#include <kern/device-io.h>
#include <kern/irq.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

/* The registers the driver uses, by their offsets. */
#define CDNS_UART_INTERRUPT_ENABLE	0x08U
#define CDNS_UART_INTERRUPT_DISABLE	0x0cU
#define CDNS_UART_INTERRUPT_STATUS	0x14U
#define CDNS_UART_RX_TIMEOUT		0x1cU
#define CDNS_UART_RX_TRIGGER_LEVEL	0x20U
#define CDNS_UART_STATUS		0x2cU
#define CDNS_UART_FIFO			0x30U

/* How many bytes of address space the registers take. */
#define CDNS_UART_WINDOW		0x1000U

/* The receive trigger and receive timeout interrupts, and every interrupt the UART has. */
#define CDNS_UART_INTERRUPT_RX_TRIGGER	0x0001U
#define CDNS_UART_INTERRUPT_RX_TIMEOUT	0x0100U
#define CDNS_UART_INTERRUPT_ALL		0x3fffU

/* The status register's bit that says the receiver holds nothing. */
#define CDNS_UART_STATUS_RX_EMPTY	0x002U

/*
 * The receive trigger level, one byte, so each byte is taken as it
 * arrives; and the receive timeout, in units of four bit times, which
 * catches a byte left below a higher level.
 */
#define CDNS_UART_TRIGGER_ONE_BYTE	1U
#define CDNS_UART_TIMEOUT_DEFAULT	10U

/* The received character in the FIFO register. */
#define CDNS_UART_FIFO_CHARACTER	0x0ffU

/*
 * How many bytes one interrupt takes.  A line that never stops delivering
 * must not keep the processor in the handler for ever; what is left raises
 * the interrupt again.
 */
#define CDNS_UART_DRAIN_LIMIT		64U

/* Marks a parameter the interrupt handler's signature carries but the driver does not need. */
#define UNUSED_PARAMETER(parameter)	((void)(parameter))

static uint32_t read_register(const struct drv_cdns_uart *port, uint32_t offset);
static void write_register(const struct drv_cdns_uart *port, uint32_t offset, uint32_t value);
static void drain_receiver(struct drv_cdns_uart *port, bool deliver);
static void uart_interrupt(int irq, kern_irq_ack_t acknowledge, void *argument);

/*
 * Maps a Cadence UART's registers and turns its interrupts off.
 */
int
drv_cdns_uart_attach(
	struct drv_cdns_uart *port,
	uint64_t physical,
	int irq)
{
	void *mapped;
	int error;

	/* Refuses a missing port and an interrupt the GIC cannot have. */
	if (port == NULL || irq <= 0)
		return EINVAL;

	/* Maps the registers, uncached as every device register is. */
	error = kern_device_map(physical, CDNS_UART_WINDOW, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0)
		return error;

	/* Fills the port; input is started separately. */
	port->physical = physical;
	port->base = (uintptr_t)mapped;
	port->irq = irq;
	port->receive = NULL;
	port->input_started = false;

	/* No interrupt until the input starts: the console's output is polled. */
	write_register(port, CDNS_UART_INTERRUPT_DISABLE, CDNS_UART_INTERRUPT_ALL);

	/* Succeeded: the UART's registers are reachable. */
	return 0;
}

/*
 * Starts taking received bytes by interrupt.
 */
int
drv_cdns_uart_start_input(
	struct drv_cdns_uart *port,
	drv_cdns_uart_receive_t receive)
{
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

	/* Raises the receive interrupt at each byte, and at a byte that waits too long. */
	write_register(port, CDNS_UART_RX_TRIGGER_LEVEL, CDNS_UART_TRIGGER_ONE_BYTE);
	write_register(port, CDNS_UART_RX_TIMEOUT, CDNS_UART_TIMEOUT_DEFAULT);

	/*
	 * Drops what the line carried before anything could read it, and
	 * every status it left raised: the first byte the reader sees must be
	 * the first one typed to it.
	 */
	drain_receiver(port, false);
	write_register(port, CDNS_UART_INTERRUPT_STATUS, CDNS_UART_INTERRUPT_ALL);

	/*
	 * Publishes the receiver before the interrupt can run: the handler
	 * delivers only once input_started is set.
	 */
	port->receive = receive;
	port->input_started = true;

	/* Has the UART raise its interrupt for a received byte and for a byte left waiting. */
	write_register(port, CDNS_UART_INTERRUPT_ENABLE, CDNS_UART_INTERRUPT_RX_TRIGGER | CDNS_UART_INTERRUPT_RX_TIMEOUT);

	/* Lets the interrupt through to the handler. */
	kern_irq_unmask(port->irq);

	/* Succeeded: received bytes go to the receiver. */
	return 0;
}

/* Reads one 32-bit register. */
static uint32_t
read_register(
	const struct drv_cdns_uart *port,
	uint32_t offset)
{
	uint32_t value;

	/* Every register of the UART is read as a word. */
	value = kern_mmio_read32((const volatile void *)(port->base + offset));

	/* Succeeded: the register's value. */
	return value;
}

/* Writes one 32-bit register. */
static void
write_register(
	const struct drv_cdns_uart *port,
	uint32_t offset,
	uint32_t value)
{
	/* Every register of the UART is written as a word. */
	kern_mmio_write32((volatile void *)(port->base + offset), value);
}

/*
 * Takes what the receiver holds, at most one interrupt's worth, and hands
 * each byte to the receiver when deliver is set.
 */
static void
drain_receiver(
	struct drv_cdns_uart *port,
	bool deliver)
{
	unsigned drained;
	uint32_t status;
	uint32_t data;

	/* Takes each byte the receiver holds. */
	for (drained = 0; drained < CDNS_UART_DRAIN_LIMIT; drained++) {
		/* Stops once the receiver holds nothing. */
		status = read_register(port, CDNS_UART_STATUS);
		if ((status & CDNS_UART_STATUS_RX_EMPTY) != 0)
			return;

		/* Takes the byte, which also frees its place in the receiver. */
		data = read_register(port, CDNS_UART_FIFO);

		/* Hands the byte on, unless it is one from before the start. */
		if (deliver)
			port->receive((uint8_t)(data & CDNS_UART_FIFO_CHARACTER));
	}
}

/*
 * Serves the UART's interrupt: clears the status it serves, hands the
 * received bytes on and ends the interrupt.
 */
static void
uart_interrupt(
	int irq,
	kern_irq_ack_t acknowledge,
	void *argument)
{
	struct drv_cdns_uart *port;
	uint32_t status;

	UNUSED_PARAMETER(irq);

	/* Finds the port the handler was installed for. */
	port = (struct drv_cdns_uart *)argument;

	/*
	 * Clears the raised status by writing it back; a byte that arrives
	 * after the drain raises the interrupt again.
	 */
	status = read_register(port, CDNS_UART_INTERRUPT_STATUS);
	write_register(port, CDNS_UART_INTERRUPT_STATUS, status);

	/* Hands what arrived to the receiver. */
	drain_receiver(port, port->input_started);

	/* Ends the interrupt at its controller. */
	kern_irq_send_eoi(acknowledge);
}
