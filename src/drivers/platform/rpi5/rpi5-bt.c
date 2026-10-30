/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Raspberry Pi 5 onboard Bluetooth: the BCM43455's HCI on the BCM2712's
 * uarta, a 16550-compatible UART (rpi5-bt.h).
 *
 * The probe reads the device tree and starts one thread.  The thread
 * routes the UART's four pins (GPIO 24 to 27) and BT_ON (GPIO 29 of the
 * gio block), sets the UART up at 115200 baud with hardware flow control,
 * pulses BT_ON, and publishes the controller through the HCI class.  From
 * then on it is the worker: it takes the bytes the interrupt handler put
 * in the receive ring, cuts them into H4 packets and hands those to the
 * class.
 *
 * Falling in step.  Every reset, every power-up and every change of the
 * line's speed is a transition: the bytes that come while it lasts and a
 * short quiet time after it are dropped, and then the worker hunts for a
 * Command Complete or Command Status answering the last command sent
 * (rpi-bt-link.c).  Only once one is found is the line in step; a header
 * no controller sends, a receive error or a packet cut short then means the
 * line lost step, and the worker resets the controller by itself (the
 * class's RESET notice tells the daemon).  More than three such resets a
 * minute stop the controller until the program resets it.
 *
 * Sending.  A packet goes whole into the send ring, the FIFO is loaded
 * once it is empty (the UART raises its send interrupt when the FIFO has
 * emptied), and send() waits, without a signal ending it, until the last
 * byte is in the FIFO.  A controller that holds the line for two seconds
 * is reset.
 *
 * Locks, in the order they are taken: the class's operations' mutex (held
 * while the class calls send, reset or set_speed), the power mutex (held
 * across a transition), the spinlock (the rings, the line's state, the
 * interrupt enables; taken by the interrupt handler too).  Nothing calls
 * the class with the spinlock held.  A transition made by an operation
 * first asks the worker to stop and waits for it to say it is parked,
 * outside the power mutex; the worker makes its own resets itself, under
 * the power mutex alone.  The line's generation tells the worker, and a
 * waiting sender, that a transition happened while they did not hold the
 * spinlock.
 *
 * The pins, the UART's interrupt and clock and the power line are the
 * board's device tree's.  The times (the quiet 10 ms, the 100 ms gap
 * within a packet, the CTS wait of 500 ms, the send's 2 s) are this
 * driver's own choices, to be measured on a board.
 */

#include "drivers/platform/rpi5/rpi5-bt.h"
#include "drivers/platform/rpi5/rpi5-gio.h"
#include "drivers/platform/rpi5/rpi5-pinctrl.h"
#include "drivers/platform/rpi-common/rpi-bt-link.h"
#include <drivers/generic/bt-hci.h>
#include <drivers/generic/fdt.h>
#include <kern/atomic.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/irq.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/pmem.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <kern/waitq.h>
#include <uapi/errno.h>

/* Names a parameter a function does not use. */
#define UNUSED_PARAMETER(parameter)	((void)(parameter))

/* How much of memory at the device tree's address may be read, the same bound the platform uses. */
#define BT_FDT_READ_LIMIT		(2U * 1024U * 1024U)

/*
 * The UART's registers, a 32-bit word each (the tree's reg-shift of 2),
 * and their bits.
 *
 * Values referenced: the 16550 UART's register map (RBR, THR, DLL, IER,
 * DLM, IIR, FCR, LCR, MCR, LSR, MSR and their bits; MCR's bit 5 is the
 * automatic RTS and CTS flow control of the 16750 and its successors).
 */
#define UART_RBR			0x00U
#define UART_THR			0x00U
#define UART_DLL			0x00U
#define UART_IER			0x04U
#define UART_DLM			0x04U
#define UART_IIR			0x08U
#define UART_FCR			0x08U
#define UART_LCR			0x0cU
#define UART_MCR			0x10U
#define UART_LSR			0x14U
#define UART_MSR			0x18U

/* How much of the register block is mapped, and the bytes the send FIFO takes at once. */
#define UART_REGISTER_SPAN		0x20U
#define UART_FIFO_BYTES			16U

/* IER: received bytes (and the receive timeout), and the send FIFO emptied. */
#define UART_IER_RX			(1U << 0)
#define UART_IER_TX			(1U << 1)

/* FCR: the FIFOs on, both emptied, and the receive interrupt at the second trigger level. */
#define UART_FCR_ENABLE			(1U << 0)
#define UART_FCR_CLEAR_RX		(1U << 1)
#define UART_FCR_CLEAR_TX		(1U << 2)
#define UART_FCR_RX_TRIGGER		(1U << 6)

/* LCR: eight bits, no parity, one stop bit; and the divisor latch. */
#define UART_LCR_8N1			0x03U
#define UART_LCR_DLAB			(1U << 7)

/* MCR: DTR and RTS up, OUT2, and the hardware's RTS and CTS flow control. */
#define UART_MCR_DTR			(1U << 0)
#define UART_MCR_RTS			(1U << 1)
#define UART_MCR_OUT2			(1U << 3)
#define UART_MCR_AUTO_FLOW		(1U << 5)

/* LSR: a byte ready, its overrun, parity, framing and break errors, the send FIFO empty, and the line idle. */
#define UART_LSR_DATA_READY		(1U << 0)
#define UART_LSR_OVERRUN		(1U << 1)
#define UART_LSR_PARITY			(1U << 2)
#define UART_LSR_FRAMING		(1U << 3)
#define UART_LSR_BREAK			(1U << 4)
#define UART_LSR_THR_EMPTY		(1U << 5)
#define UART_LSR_TX_EMPTY		(1U << 6)

/* MSR: the controller's clear to send. */
#define UART_MSR_CTS			(1U << 4)

/*
 * A receive ring entry: the byte, and above it the errors the line status
 * gave for it (1 framing, 2 parity, 4 break, 8 overrun).
 */
#define ENTRY_DATA			0x00ffU
#define ENTRY_ERRORS			0x0f00U
#define ENTRY_ERRORS_SHIFT		8U
#define ENTRY_FRAMING			(1U << 8)
#define ENTRY_PARITY			(1U << 9)
#define ENTRY_BREAK			(1U << 10)
#define ENTRY_OVERRUN			(1U << 11)

/* The UART samples each bit sixteen times, and a divisor may be off by this many thousandths. */
#define UART_OVERSAMPLE			16U
#define UART_DIVISOR_MAX		0xffffU
#define UART_ERROR_PERMILLE		15U

/* The speed the controller's ROM and the line start at after every power-up. */
#define LINE_START_BAUD			115200U

/* The device tree's interrupt: a GIC SPI, level high, at 32 past the SGIs and PPIs. */
#define FDT_GIC_SPI			0U
#define FDT_IRQ_LEVEL_HIGH		4U
#define FDT_SPI_BASE			32U
#define FDT_SPI_MAX			987U

/*
 * The times, in milliseconds: this driver's own choices (see the file's
 * comment), except the power cycle's.  BT_ON stays low for the CYW43455's
 * core buck regulator to discharge (300 ms) and the controller settles for
 * 500 ms after BT_ON rises before a command is sent.
 *
 * Values referenced: Zephyr drivers/bluetooth/hci/h4_ifx_cyw43xxx.c
 * (Apache-2.0; BT_POWER_CBUCK_DISCHARGE_TIME_MS,
 * BT_POWER_ON_SETTLING_TIME_MS).
 */
#define POWER_LOW_MS			300U
#define POWER_SETTLE_MS			500U
#define CTS_WAIT_MS			500U
#define CTS_POLL_MS			1U
#define QUIET_MS			10U
#define BYTE_GAP_MS			100U
#define PARK_WAIT_MS			2000U
#define SEND_DRAIN_MS			2000U
#define SPEED_DRAIN_MS			100U
#define BUSY_WAIT_POLLS			1000U
#define COUNTS_PERIOD_MS		600000U

/* The receive ring's entries (a byte and its error bits each), the send ring's bytes, and one batch of the worker. */
#define RX_RING_ENTRIES			8192U
#define TX_RING_BYTES			8192U
#define RX_BATCH			256U

/* How many of the first bytes each way since a reset the log keeps. */
#define SEEN_BYTES			24U

/*
 * The steps of the probe.
 *
 * NONE: not probed.  CLAIMED: one caller is probing.  RUNNING: the thread
 * was started.  REFUSED: the probe found nothing to drive.  Only the caller
 * that moves NONE to CLAIMED probes.
 */
#define PROBE_NONE			0U
#define PROBE_CLAIMED			1U
#define PROBE_RUNNING			2U
#define PROBE_REFUSED			3U

/*
 * The onboard Bluetooth: what the tree says of it, the line's rings and
 * state, and the worker's own reassembly.
 *
 * There is one per board, and it lives for as long as the kernel runs.
 * The probe fills the description (clock_hz among it) before it starts
 * the thread; the thread fills gio, registers, speed_max and hci before
 * the controller is published, and nothing changes them afterwards.
 *
 * lock guards everything between it and power_lock: the rings, the line,
 * the interrupt mask's copy, the worker's park handshake, the last command
 * sent and the counts.  The interrupt handler takes it.  Every wait queue
 * here sleeps and is woken under it.
 *
 * power_lock serializes the transitions (the power cycles, the stops and
 * the speed changes); it is held across their sleeps.
 *
 * The fields after power_lock are the worker's alone.
 */
struct rpi5_bt {
	struct drv_fdt fdt;
	uint64_t uart_physical;
	int irq;
	uint32_t tree_speed;
	uint8_t board_address[BT_ADDRESS_SIZE];
	uint32_t uart_node;
	uint32_t gio_node;
	unsigned power_pin;
	unsigned power_active_low;
	struct drv_rpi5_gio gio;
	void *registers;
	uint32_t clock_hz;
	uint32_t speed_max;
	struct drv_bt_hci *hci;

	struct spinlock lock;
	struct wait_queue worker_wake;
	struct wait_queue sender_wake;
	struct wait_queue parked_wake;
	struct wait_queue pause_wake;
	struct drv_rpi_bt_link link;
	uint32_t interrupt_mask;
	uint16_t rx_ring[RX_RING_ENTRIES];
	size_t rx_head;
	size_t rx_used;
	unsigned rx_masked;
	uint8_t tx_ring[TX_RING_BYTES];
	size_t tx_head;
	size_t tx_used;
	uint64_t tx_queued;
	uint64_t tx_written;
	uint16_t sent_opcode;
	unsigned sent_opcode_known;
	unsigned stop_request;
	unsigned parked;
	unsigned room;
	uint64_t interrupts;
	uint64_t rx_bytes;
	uint64_t rx_errors;
	uint64_t tx_bytes;
	uint64_t quiet_dropped;
	uint64_t self_resets;
	uint8_t seen_in[SEEN_BYTES];
	uint8_t seen_in_errors[SEEN_BYTES];
	unsigned seen_in_count;
	uint8_t seen_out[SEEN_BYTES];
	unsigned seen_out_count;

	struct mutex power_lock;

	struct bt_hci_assembler assembler;
	uint8_t assembly[BT_ACL_PACKET_MAX];
	struct drv_rpi4_bt_hunt hunt;
	uint8_t pending[DRV_RPI4_BT_HUNT_MAX];
	size_t pending_have;
	size_t pending_next;
	uint16_t batch[RX_BATCH];
	size_t batch_have;
	size_t batch_next;
	uint32_t worker_generation;
	enum drv_rpi4_bt_mode worker_mode;
	unsigned held;
	uint64_t last_byte_tick;
	uint64_t next_counts_tick;
	uint64_t counts_logged;
};

/*
 * How far the probe has come: one of PROBE_*.
 *
 * Moved from NONE by one compare-and-exchange, so the board is probed
 * once; it never goes back.
 */
static volatile unsigned bt_probe_state;

/* The board's one onboard Bluetooth; see struct rpi5_bt for who owns which field. */
static struct rpi5_bt bt_board;

static int bt_send(void *context, const uint8_t *packet, size_t length);
static int bt_set_bootloader(void *context, int on);
static int bt_reset(void *context);
static void bt_room(void *context);
static int bt_set_speed(void *context, uint32_t baud);

/* What the HCI class calls: no SCO until ws215-p008 (the class then answers SET_SCO with ENOTSUP). */
static const struct drv_bt_hci_ops bt_ops = {
	.send = bt_send,
	.set_bootloader = bt_set_bootloader,
	.reset = bt_reset,
	.room = bt_room,
	.sco_set = NULL,
	.sco_send = NULL,
	.set_speed = bt_set_speed
};

static int describe(struct rpi5_bt *bt);
static int node_compatible(const struct drv_fdt *fdt, uint32_t node, const char *name);
static int describe_interrupt(struct rpi5_bt *bt, uint32_t node);
static void bt_thread(void *argument);
static int attach(struct rpi5_bt *bt);
static int route_pins(struct rpi5_bt *bt);
static int install_interrupt(struct rpi5_bt *bt);
static int publish(struct rpi5_bt *bt);
static void interrupt_handler(int irq, kern_irq_ack_t acknowledge, void *argument);
static void fill_fifo_locked(struct rpi5_bt *bt);
static void write_mask_locked(struct rpi5_bt *bt, uint32_t mask);
static void begin_transition_locked(struct rpi5_bt *bt);
static void stop_line_locked(struct rpi5_bt *bt);
static int power_cycle(struct rpi5_bt *bt);
static void power_off(struct rpi5_bt *bt);
static int setup_uart(struct rpi5_bt *bt, uint32_t baud);
static void finish_quiet(struct rpi5_bt *bt);
static int park_worker(struct rpi5_bt *bt);
static void release_worker(struct rpi5_bt *bt);
static void self_reset(struct rpi5_bt *bt, uint32_t observed, const char *why, int by_worker);
static void notice_reset(struct rpi5_bt *bt);
static void worker_loop(struct rpi5_bt *bt);
static int worker_take(struct rpi5_bt *bt);
static void worker_restart_locked(struct rpi5_bt *bt, uint32_t generation, enum drv_rpi4_bt_mode mode);
static void worker_process(struct rpi5_bt *bt);
static int worker_hunt(struct rpi5_bt *bt);
static int worker_anchor(struct rpi5_bt *bt, size_t length);
static int worker_sync(struct rpi5_bt *bt);
static void worker_wait(struct rpi5_bt *bt);
static void worker_gap(struct rpi5_bt *bt);
static void lost_step(struct rpi5_bt *bt, const char *why);
static void log_counts(struct rpi5_bt *bt, const char *when, int only_if_moved);
static void log_seen(struct rpi5_bt *bt);
static int deliver(void *context, uint8_t type, const uint8_t *packet, size_t length);
static uint32_t read_register(struct rpi5_bt *bt, uint32_t offset);
static void write_register(struct rpi5_bt *bt, uint32_t offset, uint32_t value);
static void pause_ms(struct rpi5_bt *bt, unsigned milliseconds);
static uint64_t now_ms(void);
static int set_power(struct rpi5_bt *bt, int on);
static uint32_t entry_errors(uint32_t status);
static int uart_divisor(uint32_t clock_hz, uint32_t baud, uint32_t *divisor);
static uint32_t uart_speed_limit(uint32_t clock_hz, uint32_t tree_max);

/*
 * Looks for the onboard Bluetooth and starts its thread.
 */
int
drv_rpi5_bt_probe(
	uint64_t fdt_phys)
{
	struct rpi5_bt *bt;
	struct thread *thread;
	const void *blob;
	unsigned expected;
	int claimed;
	int error;

	/* Probes once: a second call finds the first one's result. */
	expected = PROBE_NONE;
	claimed = atomic_raw_compare_exchange(&bt_probe_state, &expected, PROBE_CLAIMED);
	if (!claimed)
		return EALREADY;
	bt = &bt_board;

	/* Finds the device tree, which stays in the direct map for as long as the kernel runs. */
	if (fdt_phys == 0) {
		atomic_raw_store_release(&bt_probe_state, PROBE_REFUSED);
		return ENODEV;
	}

	/* Maps the tree's address. */
	blob = kern_pmem_to_kernel((kern_physaddr_t)fdt_phys);
	if (blob == NULL) {
		atomic_raw_store_release(&bt_probe_state, PROBE_REFUSED);
		return ENODEV;
	}

	/* Opens the tree, checking its header. */
	error = drv_fdt_open(&bt->fdt, blob, BT_FDT_READ_LIMIT);
	if (error != 0) {
		atomic_raw_store_release(&bt_probe_state, PROBE_REFUSED);
		return error;
	}

	/* Finds the controller's node under the UART. */
	error = describe(bt);
	if (error != 0) {
		kern_logf("rpi5-bt: no enabled Bluetooth node on the UART (%d)\n", error);
		atomic_raw_store_release(&bt_probe_state, PROBE_REFUSED);
		return error;
	}

	/* The locks and the queues the thread, the interrupt and the class share. */
	error = mutex_init(&bt->power_lock, LOCK_RANK_DEVICE, "rpi5 bt power");
	if (error != 0) {
		atomic_raw_store_release(&bt_probe_state, PROBE_REFUSED);
		return error;
	}

	/* The lock of the line, and its wait queues. */
	spin_init(&bt->lock, LOCK_RANK_DEVICE, "rpi5 bt");
	waitq_init(&bt->worker_wake, "rpi5 bt worker");
	waitq_init(&bt->sender_wake, "rpi5 bt sender");
	waitq_init(&bt->parked_wake, "rpi5 bt parked");
	waitq_init(&bt->pause_wake, "rpi5 bt pause");
	drv_rpi_bt_link_init(&bt->link);

	/* Creates the thread that does everything that touches the hardware. */
	error = kthread_create(bt_thread, bt, SCHED_PRIORITY_DEFAULT, &thread);
	if (error != 0) {
		kern_logf("rpi5-bt: thread not created (%d)\n", error);
		atomic_raw_store_release(&bt_probe_state, PROBE_REFUSED);
		return error;
	}

	/* RUNNING records that the thread owns the board from now on. */
	atomic_raw_store_release(&bt_probe_state, PROBE_RUNNING);

	/* Starts the thread; nobody joins it, it runs for as long as the kernel does. */
	thread->detached = 1;
	thread_start(thread);

	/* Succeeded: the thread brings the controller up in the background. */
	return 0;
}

/*
 * Finds the controller in the device tree: an enabled brcm,bcm43438-bt
 * node (the name the tree gives the BCM43455 too) whose parent is an
 * enabled BCM7271 UART.  Reads the UART's registers, interrupt and clock,
 * the line's highest speed, the board's address and the controller's
 * power line.
 */
static int
describe(
	struct rpi5_bt *bt)
{
	const uint8_t *value;
	uint32_t length;
	uint32_t node;
	uint32_t parent;
	uint32_t phandle;
	uint64_t size;
	bool enabled;
	int uart;
	int error;

	/* Each node of the controller's name, until one hangs off an enabled BCM7271 UART. */
	node = DRV_FDT_NO_NODE;
	for (;;) {
		error = drv_fdt_find_compatible(&bt->fdt, "brcm,bcm43438-bt", node, &node);
		if (error != 0)
			return ENODEV;

		/* The node's parent, which must be the UART. */
		error = drv_fdt_parent(&bt->fdt, node, &parent);
		if (error != 0)
			continue;
		uart = node_compatible(&bt->fdt, parent, "brcm,bcm7271-uart");
		if (!uart)
			continue;

		/* The UART enabled. */
		enabled = drv_fdt_node_enabled(&bt->fdt, parent);
		if (!enabled)
			continue;

		/* The controller enabled. */
		enabled = drv_fdt_node_enabled(&bt->fdt, node);
		if (!enabled)
			continue;

		/* This is the controller. */
		break;
	}

	/* Keeps the UART's node for its pins. */
	bt->uart_node = parent;

	/* The UART's registers, translated to the ARM's addresses. */
	error = drv_fdt_reg(&bt->fdt, parent, 0, &bt->uart_physical, &size);
	if (error != 0)
		return EINVAL;
	if (size < UART_REGISTER_SPAN)
		return EINVAL;

	/* Its interrupt. */
	error = describe_interrupt(bt, parent);
	if (error != 0)
		return error;

	/* Its fixed clock. */
	bt->clock_hz = drv_fdt_node_cells(&bt->fdt, parent, "clock-frequency", 0);
	if (bt->clock_hz == 0)
		return EINVAL;

	/* The line's highest speed (0 for none given). */
	bt->tree_speed = 0U;
	error = drv_fdt_property(&bt->fdt, node, "max-speed", &value, &length);
	if (error == 0 && length == 4U)
		bt->tree_speed = (uint32_t)drv_fdt_cells_value(value, 0, 1);

	/* The board's address, in the HCI's order as the binding has it; all zero when the firmware left none. */
	kern_memset(bt->board_address, 0, sizeof(bt->board_address));
	error = drv_fdt_property(&bt->fdt, node, "local-bd-address", &value, &length);
	if (error == 0 && length == BT_ADDRESS_SIZE)
		kern_memcpy(bt->board_address, value, BT_ADDRESS_SIZE);

	/* The power line: shutdown-gpios, a pin of a GPIO block, active high when its flag is clear. */
	error = drv_fdt_property(&bt->fdt, node, "shutdown-gpios", &value, &length);
	if (error != 0)
		return error;
	if (length != 12U)
		return EINVAL;
	phandle = (uint32_t)drv_fdt_cells_value(value, 0, 1);
	error = drv_fdt_find_phandle(&bt->fdt, phandle, &bt->gio_node);
	if (error != 0)
		return error;
	bt->power_pin = (unsigned)drv_fdt_cells_value(value, 1, 1);
	bt->power_active_low = (unsigned)drv_fdt_cells_value(value, 2, 1) & 1U;

	/* Succeeded: the controller is on this UART. */
	return 0;
}

/* Tells whether a node's compatible list names a string. */
static int
node_compatible(
	const struct drv_fdt *fdt,
	uint32_t node,
	const char *name)
{
	const uint8_t *value;
	uint32_t length;
	uint32_t offset;
	size_t name_length;
	size_t entry_length;
	int error;

	/* A node without the list names nothing. */
	error = drv_fdt_property(fdt, node, "compatible", &value, &length);
	if (error != 0)
		return 0;

	/* Each NUL-ended string of the list in turn. */
	name_length = kern_strlen(name);
	offset = 0U;
	while (offset < length) {
		/* The entry's length, up to its NUL or the list's end. */
		entry_length = 0U;
		while (offset + entry_length < length && value[offset + entry_length] != '\0')
			entry_length++;

		/* The same string. */
		if (entry_length == name_length) {
			error = kern_memcmp(value + offset, name, name_length);
			if (error == 0)
				return 1;
		}

		/* The next entry, past this one's NUL. */
		offset += (uint32_t)entry_length + 1U;
	}

	/* Not in the list. */
	return 0;
}

/* Reads the UART's interrupt: a level-high GIC SPI, numbered for the kernel 32 past the SGIs and PPIs. */
static int
describe_interrupt(
	struct rpi5_bt *bt,
	uint32_t node)
{
	const uint8_t *value;
	uint32_t length;
	uint64_t cell;
	int error;

	/* Requires the three cells of the GIC binding. */
	error = drv_fdt_property(&bt->fdt, node, "interrupts", &value, &length);
	if (error != 0)
		return EINVAL;
	if (length < 12U)
		return EINVAL;

	/* Accepts a shared peripheral interrupt only. */
	cell = drv_fdt_cells_value(value, 0, 1);
	if (cell != FDT_GIC_SPI)
		return EINVAL;

	/* Restricts the SPI to the GIC's range. */
	cell = drv_fdt_cells_value(value, 1, 1);
	if (cell > FDT_SPI_MAX)
		return EINVAL;
	bt->irq = (int)cell + (int)FDT_SPI_BASE;

	/* Accepts the level-high line the handler is installed for. */
	cell = drv_fdt_cells_value(value, 2, 1);
	if (cell != FDT_IRQ_LEVEL_HIGH)
		return EINVAL;

	/* Succeeded: irq names the UART's line. */
	return 0;
}

/* Brings the controller up, publishes it, and serves its line from then on. */
static void
bt_thread(
	void *argument)
{
	struct rpi5_bt *bt;
	unsigned long enabled;
	int error;

	bt = argument;

	/* Opens the power line, routes the pins, sets the UART up and installs its interrupt. */
	error = attach(bt);
	if (error != 0) {
		kern_logf("rpi5-bt: not attached (%d)\n", error);
		return;
	}

	/* Powers the controller up for the first time. */
	mutex_lock(&bt->power_lock);

	enabled = spin_lock_irqsave(&bt->lock);

	begin_transition_locked(bt);

	spin_unlock_irqrestore(&bt->lock, enabled);

	/* The power cycle; a controller that does not come up is powered down again. */
	error = power_cycle(bt);
	if (error != 0)
		power_off(bt);

	mutex_unlock(&bt->power_lock);

	/* A controller that did not come up is left powered down, unpublished. */
	if (error != 0) {
		kern_logf("rpi5-bt: controller did not come up (%d)\n", error);
		return;
	}

	/* Publishes the controller; the daemon finds /dev/bluetoothN. */
	error = publish(bt);
	if (error != 0) {
		mutex_lock(&bt->power_lock);
		power_off(bt);
		mutex_unlock(&bt->power_lock);
		kern_logf("rpi5-bt: controller not published (%d)\n", error);
		return;
	}

	/* Serves the line for as long as the kernel runs. */
	worker_loop(bt);
}

/*
 * Prepares everything but the power: opens the power line's GPIO block,
 * routes the pins, maps and stops the UART, and installs its interrupt.
 */
static int
attach(
	struct rpi5_bt *bt)
{
	int level;
	int output;
	int error;

	/* Opens the GPIO block of BT_ON. */
	error = drv_rpi5_gio_open(&bt->fdt, bt->gio_node, &bt->gio);
	if (error != 0) {
		kern_logf("rpi5-bt: BT_ON's GPIO block unavailable (%d)\n", error);
		return error;
	}

	/* Records how the firmware left the power line. */
	level = 0;
	output = 0;
	error = drv_rpi5_gio_get(&bt->gio, bt->power_pin, &level, &output);
	kern_logf("rpi5-bt: BT_ON (gio %u) was level %d, %s (%d)\n", bt->power_pin, level, output ? "output" : "input", error);

	/* The fastest line both the UART and the board take. */
	bt->speed_max = uart_speed_limit(bt->clock_hz, bt->tree_speed);

	/* Routes the UART's pins and BT_ON. */
	error = route_pins(bt);
	if (error != 0)
		return error;

	/* Maps the UART's registers. */
	error = kern_device_map(bt->uart_physical, UART_REGISTER_SPAN, KERN_DEVICE_UNCACHED, &bt->registers);
	if (error != 0) {
		kern_logf("rpi5-bt: UART registers not mapped (%d)\n", error);
		return error;
	}

	/* Stops the UART with every interrupt masked and RTS down, as the line is until the power-up. */
	write_register(bt, UART_IER, 0U);
	write_register(bt, UART_MCR, 0U);
	write_register(bt, UART_FCR, UART_FCR_ENABLE | UART_FCR_CLEAR_RX | UART_FCR_CLEAR_TX);
	bt->interrupt_mask = 0U;

	/* Installs the handler on the UART's line. */
	error = install_interrupt(bt);
	if (error != 0)
		return error;

	/* Records what was found, for the first bring-up on a board. */
	kern_logf(
		"rpi5-bt: UART at %llx, irq %d, clock %u Hz, speed up to %u baud\n",
		(unsigned long long)bt->uart_physical,
		bt->irq,
		bt->clock_hz,
		bt->speed_max);

	/* Succeeded: the controller can be powered up. */
	return 0;
}

/* Gives the UART's pins and BT_ON their functions and pulls, as the UART's pin groups say. */
static int
route_pins(
	struct rpi5_bt *bt)
{
	int error;

	/* Finds the pin controller. */
	error = drv_rpi5_pinctrl_init(&bt->fdt);
	if (error != 0)
		return error;

	/* Applies the UART's groups: its four pins and BT_ON. */
	error = drv_rpi5_pinctrl_apply_default(&bt->fdt, bt->uart_node);
	if (error != 0) {
		kern_logf("rpi5-bt: pins not routed (%d)\n", error);
		return error;
	}

	/* Succeeded: the line reaches the controller. */
	kern_logf("rpi5-bt: UART and BT_ON pins routed\n");
	return 0;
}

/* Installs the interrupt handler on the UART's line, level high, and lets the line in. */
static int
install_interrupt(
	struct rpi5_bt *bt)
{
	int error;

	/* Holds the line off while it is set up (the early console left it masked). */
	kern_irq_mask(bt->irq);

	/* The UART raises its line by level, active high. */
	error = kern_irq_set_mode(bt->irq, KERN_IRQ_TRIGGER_LEVEL, KERN_IRQ_POLARITY_HIGH);
	if (error != 0) {
		kern_logf("rpi5-bt: irq %d mode not set (%d)\n", bt->irq, error);
		return error;
	}

	/* The handler. */
	error = kern_irq_register(bt->irq, interrupt_handler, bt);
	if (error != 0) {
		kern_logf("rpi5-bt: irq %d not registered (%d)\n", bt->irq, error);
		return error;
	}

	/* The line is let in; the UART's own enables hold every interrupt off until the power-up. */
	kern_irq_unmask(bt->irq);

	/* Succeeded: the handler serves the line. */
	return 0;
}

/* Publishes the controller through the HCI class. */
static int
publish(
	struct rpi5_bt *bt)
{
	struct drv_bt_hci_description description;
	struct drv_bt_hci *hci;
	unsigned long enabled;
	int error;

	/* What the controller is: a UART controller with a line speed to change and maybe a board address. */
	kern_memset(&description, 0, sizeof(description));
	description.bus = BT_BUS_UART;
	description.speed_max = bt->speed_max;
	kern_memcpy(description.board_address, bt->board_address, BT_ADDRESS_SIZE);
	description.name = "Raspberry Pi 5 Bluetooth";
	description.physical_path = "uarta";

	/* The class publishes bluetoothN. */
	hci = NULL;
	error = drv_bt_hci_register(&description, &bt_ops, bt, &hci);
	if (error != 0)
		return error;

	/* The operations reach the class's record through hci from now on. */
	enabled = spin_lock_irqsave(&bt->lock);

	bt->hci = hci;

	spin_unlock_irqrestore(&bt->lock, enabled);

	/* Succeeded: the controller is published. */
	return 0;
}

/*
 * Serves the UART's interrupt: takes every received byte with its error
 * bits into the receive ring, refills the send FIFO from the send ring,
 * and reads the interrupt's cause, which clears a send interrupt.
 */
static void
interrupt_handler(
	int irq,
	kern_irq_ack_t acknowledge,
	void *argument)
{
	struct rpi5_bt *bt;
	unsigned long enabled;
	uint32_t status;
	uint32_t entry;
	size_t tail;
	unsigned received;

	UNUSED_PARAMETER(irq);

	bt = argument;

	/* The line's work, under the lock the worker and the senders share. */
	received = 0U;
	enabled = spin_lock_irqsave(&bt->lock);

	(void)read_register(bt, UART_IIR);
	bt->interrupts++;

	/* Empties the receive FIFO into the ring while the ring has room. */
	for (;;) {
		status = read_register(bt, UART_LSR);
		if ((status & UART_LSR_DATA_READY) == 0U)
			break;

		/*
		 * A full ring masks the receive interrupt: the FIFO fills, RTS
		 * drops, and the controller holds its bytes until the worker
		 * makes room.
		 */
		if (bt->rx_used == RX_RING_ENTRIES) {
			bt->rx_masked = 1U;
			write_mask_locked(bt, bt->interrupt_mask & ~UART_IER_RX);
			break;
		}

		/* The byte and the errors the status gave for it, one entry. */
		entry = read_register(bt, UART_RBR) & ENTRY_DATA;
		entry |= entry_errors(status);
		tail = (bt->rx_head + bt->rx_used) % RX_RING_ENTRIES;
		bt->rx_ring[tail] = (uint16_t)entry;
		bt->rx_used++;
		bt->rx_bytes++;
		if ((entry & ENTRY_ERRORS) != 0U)
			bt->rx_errors++;
		received = 1U;

		/* The first bytes since the program's reset and their error bits, for its log lines. */
		if (bt->seen_in_count < SEEN_BYTES) {
			bt->seen_in[bt->seen_in_count] = (uint8_t)entry;
			bt->seen_in_errors[bt->seen_in_count] = (uint8_t)((entry & ENTRY_ERRORS) >> ENTRY_ERRORS_SHIFT);
		}

		/* Counted on past them, until the count would wrap. */
		if (bt->seen_in_count != ~0U)
			bt->seen_in_count++;
	}

	/* Refills the send FIFO; the last byte in wakes the sender. */
	fill_fifo_locked(bt);

	/* The worker has bytes to cut into packets. */
	if (received)
		waitq_wake_all(&bt->worker_wake);

	spin_unlock_irqrestore(&bt->lock, enabled);

	/* Ends the interrupt at the controller of the GIC. */
	kern_irq_send_eoi(acknowledge);
}

/*
 * Moves bytes of the send ring into the FIFO when it is empty, up to a
 * FIFO's worth; a ring with more asks for the send interrupt, an empty
 * ring masks it and wakes the sender.  The caller holds the lock.
 */
static void
fill_fifo_locked(
	struct rpi5_bt *bt)
{
	uint32_t status;
	unsigned loaded;

	/* Loads the FIFO only once it has emptied, which the status tells. */
	status = read_register(bt, UART_LSR);
	if ((status & UART_LSR_THR_EMPTY) != 0U) {
		/* Each byte, up to the FIFO's size. */
		for (loaded = 0U; loaded < UART_FIFO_BYTES && bt->tx_used != 0U; loaded++) {
			write_register(bt, UART_THR, bt->tx_ring[bt->tx_head]);

			/* The first bytes since the program's reset, for its log line. */
			if (bt->seen_out_count < SEEN_BYTES)
				bt->seen_out[bt->seen_out_count] = bt->tx_ring[bt->tx_head];
			if (bt->seen_out_count != ~0U)
				bt->seen_out_count++;
			bt->tx_head = (bt->tx_head + 1U) % TX_RING_BYTES;
			bt->tx_used--;
			bt->tx_written++;
			bt->tx_bytes++;
		}
	}

	/* More to send: the FIFO's emptying asks for the rest. */
	if (bt->tx_used != 0U) {
		write_mask_locked(bt, bt->interrupt_mask | UART_IER_TX);
		return;
	}

	/* Everything is in the FIFO: no more send interrupts, and the sender may return. */
	if ((bt->interrupt_mask & UART_IER_TX) != 0U)
		write_mask_locked(bt, bt->interrupt_mask & ~UART_IER_TX);
	waitq_wake_all(&bt->sender_wake);
}

/* Sets the UART's interrupt enables and their copy; the caller holds the lock. */
static void
write_mask_locked(
	struct rpi5_bt *bt,
	uint32_t mask)
{
	/* The copy is what the handler and the worker read. */
	bt->interrupt_mask = mask;
	write_register(bt, UART_IER, mask);
}

/*
 * Begins a transition under the lock: a new generation, every byte dropped
 * from now on, the send ring emptied (a waiting sender sees the generation
 * and answers EIO), and the receive ring emptied.
 */
static void
begin_transition_locked(
	struct rpi5_bt *bt)
{
	/* The new generation and the quiet line. */
	drv_rpi_bt_link_transition_begin(&bt->link);

	/* What was queued for the old line is dropped, and counted as sent so no wait is left for it. */
	bt->tx_head = 0U;
	bt->tx_used = 0U;
	bt->tx_written = bt->tx_queued;
	bt->quiet_dropped += bt->rx_used;
	bt->rx_head = 0U;
	bt->rx_used = 0U;

	/* No command was sent on the new line yet, and no byte seen. */
	bt->sent_opcode_known = 0U;
	bt->seen_in_count = 0U;
	bt->seen_out_count = 0U;

	/* A sender waiting for the old line gives up. */
	waitq_wake_all(&bt->sender_wake);
}

/* Stops the line under the lock: every interrupt masked and both rings dropped, until the program resets it. */
static void
stop_line_locked(
	struct rpi5_bt *bt)
{
	/* The stopped line, at a new generation. */
	drv_rpi_bt_link_stop(&bt->link);

	/* Nothing is received or sent while it is stopped. */
	write_mask_locked(bt, 0U);
	bt->rx_masked = 0U;
	bt->tx_head = 0U;
	bt->tx_used = 0U;
	bt->tx_written = bt->tx_queued;
	bt->rx_head = 0U;
	bt->rx_used = 0U;
	bt->sent_opcode_known = 0U;

	/* A sender waiting gives up. */
	waitq_wake_all(&bt->sender_wake);
}

/*
 * Turns the controller's power off and on with the line at 115200 baud,
 * receiving (so RTS is up) before BT_ON rises, waits for the controller's
 * CTS, and ends the transition after the quiet time.  The caller holds the
 * power mutex and has begun the transition.
 */
static int
power_cycle(
	struct rpi5_bt *bt)
{
	unsigned waited;
	uint32_t status;
	int error;

	/* The line as the controller's ROM starts it, receiving. */
	error = setup_uart(bt, LINE_START_BAUD);
	if (error != 0)
		return error;

	/* Holds the controller in reset. */
	error = set_power(bt, 0);
	if (error != 0) {
		kern_logf("rpi5-bt: BT_ON not turned off (%d)\n", error);
		return error;
	}

	/* Keeps the reset long enough for the controller to drain. */
	pause_ms(bt, POWER_LOW_MS);

	/* Releases the reset. */
	error = set_power(bt, 1);
	if (error != 0) {
		kern_logf("rpi5-bt: BT_ON not turned on (%d)\n", error);
		return error;
	}

	/* Waits for the controller to say it can take bytes (its RTS, our CTS). */
	waited = 0U;
	for (;;) {
		status = read_register(bt, UART_MSR);
		if ((status & UART_MSR_CTS) != 0U)
			break;
		if (waited >= CTS_WAIT_MS) {
			kern_logf("rpi5-bt: controller's CTS did not rise in %u ms\n", CTS_WAIT_MS);
			return ETIMEDOUT;
		}

		/* Looks again a millisecond later. */
		pause_ms(bt, CTS_POLL_MS);
		waited += CTS_POLL_MS;
	}

	/* Records how long the controller took, for the first bring-up on a board. */
	kern_logf("rpi5-bt: controller ready after about %u ms\n", waited);

	/* Lets the controller settle the rest of the power-on time before the first command. */
	if (waited < POWER_SETTLE_MS)
		pause_ms(bt, POWER_SETTLE_MS - waited);

	/* The quiet time, then the hunt. */
	finish_quiet(bt);

	/* Succeeded: the controller runs its ROM at 115200 baud. */
	return 0;
}

/* Powers the controller down and stops the line; the caller holds the power mutex. */
static void
power_off(
	struct rpi5_bt *bt)
{
	unsigned long enabled;
	int error;

	/* The line stops first, so nothing it receives meanwhile is taken. */
	enabled = spin_lock_irqsave(&bt->lock);

	stop_line_locked(bt);

	spin_unlock_irqrestore(&bt->lock, enabled);

	/* RTS down and the FIFOs emptied, then the controller off. */
	write_register(bt, UART_MCR, 0U);
	write_register(bt, UART_FCR, UART_FCR_ENABLE | UART_FCR_CLEAR_RX | UART_FCR_CLEAR_TX);
	error = set_power(bt, 0);
	if (error != 0)
		kern_logf("rpi5-bt: BT_ON not turned off to stop (%d)\n", error);
}

/*
 * Sets the UART to a speed: masks its interrupts, waits for the character
 * on the line, empties the FIFOs, writes the divisor with the divisor
 * latch open, sets eight bits, no parity and one stop bit, and starts it
 * again receiving with RTS up and the hardware's flow control.  The caller
 * holds the power mutex and has begun the transition.
 *
 * Procedure referenced: the 16550 UART's programming sequence (divisor
 * latch through LCR's DLAB, FIFO control, MCR's automatic flow control).
 */
static int
setup_uart(
	struct rpi5_bt *bt,
	uint32_t baud)
{
	unsigned long enabled;
	uint32_t divisor;
	uint32_t status;
	unsigned polls;
	int error;

	/* The divisor for the clock. */
	error = uart_divisor(bt->clock_hz, baud, &divisor);
	if (error != 0)
		return error;

	/* Masks the interrupts while the UART changes; the handler leaves it alone. */
	enabled = spin_lock_irqsave(&bt->lock);

	write_mask_locked(bt, 0U);
	bt->rx_masked = 0U;

	spin_unlock_irqrestore(&bt->lock, enabled);

	/* Waits for the character on the line to leave. */
	for (polls = 0U; polls < BUSY_WAIT_POLLS; polls++) {
		status = read_register(bt, UART_LSR);
		if ((status & UART_LSR_TX_EMPTY) != 0U)
			break;
		kern_usleep_range(10U, 20U);
	}

	/* Empties both FIFOs and sets the receive interrupt's level. */
	write_register(bt, UART_FCR, UART_FCR_ENABLE | UART_FCR_CLEAR_RX | UART_FCR_CLEAR_TX | UART_FCR_RX_TRIGGER);

	/* Writes the divisor through the latch, then the character format. */
	write_register(bt, UART_LCR, UART_LCR_DLAB | UART_LCR_8N1);
	write_register(bt, UART_DLL, divisor & 0xffU);
	write_register(bt, UART_DLM, (divisor >> 8) & 0xffU);
	write_register(bt, UART_LCR, UART_LCR_8N1);

	/* Clears the errors and whatever the FIFO still held. */
	for (polls = 0U; polls < UART_FIFO_BYTES * 4U; polls++) {
		status = read_register(bt, UART_LSR);
		if ((status & UART_LSR_DATA_READY) == 0U)
			break;
		(void)read_register(bt, UART_RBR);
	}

	/* Reads the interrupt and modem status, clearing what they held. */
	(void)read_register(bt, UART_IIR);
	(void)read_register(bt, UART_MSR);

	/* Starts receiving with RTS up and the hardware's RTS and CTS flow control. */
	write_register(bt, UART_MCR, UART_MCR_DTR | UART_MCR_RTS | UART_MCR_OUT2 | UART_MCR_AUTO_FLOW);
	kern_logf("rpi5-bt: the line at %u baud: LCR %02x, MCR %02x, LSR %02x, MSR %02x, divisor %u (clock %u Hz)\n",
		  (unsigned)baud,
		  (unsigned)read_register(bt, UART_LCR),
		  (unsigned)read_register(bt, UART_MCR),
		  (unsigned)read_register(bt, UART_LSR),
		  (unsigned)read_register(bt, UART_MSR),
		  (unsigned)divisor,
		  (unsigned)bt->clock_hz);

	/* Receives again: the FIFO is read, so RTS stays up. */
	enabled = spin_lock_irqsave(&bt->lock);

	write_mask_locked(bt, UART_IER_RX);

	spin_unlock_irqrestore(&bt->lock, enabled);

	/* Succeeded: the line runs at the speed. */
	return 0;
}

/*
 * Ends a transition: lets the quiet time pass, drops what came during it,
 * and starts the hunt.  The caller holds the power mutex.
 */
static void
finish_quiet(
	struct rpi5_bt *bt)
{
	unsigned long enabled;

	/* The bytes of the change itself, and those just after it, are garbage. */
	pause_ms(bt, QUIET_MS);

	/* Drops them, lets a masked receiver in again, and hunts. */
	enabled = spin_lock_irqsave(&bt->lock);

	bt->quiet_dropped += bt->rx_used;
	bt->rx_head = 0U;
	bt->rx_used = 0U;
	if (bt->rx_masked) {
		bt->rx_masked = 0U;
		write_mask_locked(bt, bt->interrupt_mask | UART_IER_RX);
	}

	/* The quiet line hunts now, and the worker finds it so. */
	drv_rpi_bt_link_transition_end(&bt->link);
	waitq_wake_all(&bt->worker_wake);

	spin_unlock_irqrestore(&bt->lock, enabled);
}

/*
 * Asks the worker to stop and waits until it says it is parked, so an
 * operation's transition does not race the worker's use of the line.  The
 * caller does not hold the power mutex (the worker may need it to finish a
 * reset of its own).  Returns 0, or ETIMEDOUT (the request withdrawn).
 */
static int
park_worker(
	struct rpi5_bt *bt)
{
	unsigned long enabled;
	uint64_t deadline;
	uint64_t observed;
	int error;

	/* The worker sees stop_request at the top of its loop and parks. */
	deadline = sched_ticks() + kern_ms_to_ticks(PARK_WAIT_MS);
	error = 0;
	enabled = spin_lock_irqsave(&bt->lock);

	bt->stop_request = 1U;
	waitq_wake_all(&bt->worker_wake);
	while (!bt->parked) {
		observed = waitq_sequence(&bt->parked_wake);
		error = waitq_sleep(&bt->parked_wake, &bt->lock, observed, deadline, 0);
		if (error == ETIMEDOUT && !bt->parked) {
			bt->stop_request = 0U;
			waitq_wake_all(&bt->worker_wake);
			break;
		}

		/* A spurious wakeup: the sleep goes on. */
		error = 0;
	}

	spin_unlock_irqrestore(&bt->lock, enabled);

	/* Reports a worker that did not park in time. */
	if (error != 0) {
		kern_logf("rpi5-bt: worker did not park in %u ms\n", PARK_WAIT_MS);
		return error;
	}

	/* Succeeded: the worker waits until it is released. */
	return 0;
}

/* Lets the parked worker go on. */
static void
release_worker(
	struct rpi5_bt *bt)
{
	unsigned long enabled;

	/* Withdraws the request and wakes the worker. */
	enabled = spin_lock_irqsave(&bt->lock);

	bt->stop_request = 0U;
	waitq_wake_all(&bt->worker_wake);

	spin_unlock_irqrestore(&bt->lock, enabled);
}

/*
 * Resets the controller because the line lost step (the worker) or a send
 * stalled (a sender), unless the line was reset since observed; stops the
 * controller instead after too many resets.  A sender parks the worker
 * first; the worker is its own.  The class is told with the RESET notice.
 */
static void
self_reset(
	struct rpi5_bt *bt,
	uint32_t observed,
	const char *why,
	int by_worker)
{
	unsigned long enabled;
	uint64_t now;
	int decision;
	int error;

	/* A sender keeps the worker away from the line meanwhile. */
	if (!by_worker) {
		error = park_worker(bt);
		if (error != 0)
			return;
	}

	/* Decides under the power mutex, so no other transition comes between the decision and the reset. */
	mutex_lock(&bt->power_lock);

	enabled = spin_lock_irqsave(&bt->lock);

	/* Asks the history of resets whether this one goes ahead, stops the controller for good, or is cancelled. */
	now = now_ms();
	decision = drv_rpi_bt_link_self_reset(&bt->link, observed, now);
	if (decision == DRV_RPI4_BT_SELF_RESET_GO) {
		bt->self_resets++;
		begin_transition_locked(bt);
	}

	spin_unlock_irqrestore(&bt->lock, enabled);

	/* The reset, or the stop; a cancelled one leaves the line as the other reset left it. */
	error = 0;
	if (decision == DRV_RPI4_BT_SELF_RESET_GO) {
		error = power_cycle(bt);
		if (error != 0)
			power_off(bt);
	} else if (decision == DRV_RPI4_BT_SELF_RESET_STOP) {
		power_off(bt);
	}

	mutex_unlock(&bt->power_lock);

	/*
	 * Records why, and tells the daemon its controller's state is gone,
	 * before the worker can deliver anything of the new line.
	 */
	if (decision == DRV_RPI4_BT_SELF_RESET_STOP) {
		kern_logf("rpi5-bt: %s; too many resets, controller stopped until reset\n", why);
	} else if (decision == DRV_RPI4_BT_SELF_RESET_GO && error != 0) {
		kern_logf("rpi5-bt: %s; reset failed (%d), controller stopped\n", why, error);
	} else if (decision == DRV_RPI4_BT_SELF_RESET_GO) {
		kern_logf("rpi5-bt: %s; controller reset\n", why);
	}

	/* The notice and the counts for a reset made or a stop. */
	if (decision != DRV_RPI4_BT_SELF_RESET_CANCEL) {
		log_counts(bt, "after a reset", 0);
		notice_reset(bt);
	}

	/* The sender's worker goes on. */
	if (!by_worker)
		release_worker(bt);
}

/* Queues the class's RESET notice for the program, once the controller is published. */
static void
notice_reset(
	struct rpi5_bt *bt)
{
	struct drv_bt_hci *hci;
	unsigned long enabled;

	/* The class's record. */
	enabled = spin_lock_irqsave(&bt->lock);

	hci = bt->hci;

	spin_unlock_irqrestore(&bt->lock, enabled);

	/* Nothing to tell before the controller is published. */
	if (hci == NULL)
		return;

	/* The program's packets of the old controller end here. */
	drv_bt_hci_notice(hci, BT_PACKET_NOTICE_RESET);
}

/*
 * Sends one checked H4 packet: whole into the send ring and as much as
 * fits into the FIFO at once, then waits (no signal ends it) until its
 * last byte is in the FIFO.  The class sends one packet at a time, and
 * each send waits for its last byte, so the ring is empty when a send
 * begins.
 */
static int
bt_send(
	void *context,
	const uint8_t *packet,
	size_t length)
{
	struct rpi5_bt *bt;
	unsigned long enabled;
	uint64_t deadline;
	uint64_t observed_sequence;
	uint64_t end;
	uint32_t observed;
	size_t tail;
	size_t index;
	unsigned delivered;
	unsigned same_line;
	int error;

	bt = context;

	/* Queues the packet on a running line, then waits for its last byte. */
	enabled = spin_lock_irqsave(&bt->lock);

	/* A stopped line, or one being reset by the worker, takes nothing. */
	if (bt->link.mode != DRV_RPI4_BT_MODE_HUNT && bt->link.mode != DRV_RPI4_BT_MODE_SYNC) {
		spin_unlock_irqrestore(&bt->lock, enabled);

		return EIO;
	}

	/* The ring is empty here unless the invariant above broke. */
	if (TX_RING_BYTES - bt->tx_used < length) {
		spin_unlock_irqrestore(&bt->lock, enabled);

		return EIO;
	}

	/* The last command sent, whose answer the hunt waits for. */
	observed = bt->link.generation;
	if (packet[0] == BT_PACKET_COMMAND && length >= 3U) {
		bt->sent_opcode = (uint16_t)((unsigned)packet[1] | ((unsigned)packet[2] << 8));
		bt->sent_opcode_known = 1U;
	}

	/* The whole packet into the ring, never cut. */
	for (index = 0U; index < length; index++) {
		tail = (bt->tx_head + bt->tx_used) % TX_RING_BYTES;
		bt->tx_ring[tail] = packet[index];
		bt->tx_used++;
	}

	/* The packet's end, which the FIFO's written count must reach. */
	bt->tx_queued += length;
	end = bt->tx_queued;

	/* Fills the FIFO now: its send interrupt comes only as it drains. */
	fill_fifo_locked(bt);

	/* Waits for the last byte, or for a reset of the line, at most SEND_DRAIN_MS. */
	deadline = sched_ticks() + kern_ms_to_ticks(SEND_DRAIN_MS);
	while (bt->tx_written < end && bt->link.generation == observed) {
		observed_sequence = waitq_sequence(&bt->sender_wake);
		error = waitq_sleep(&bt->sender_wake, &bt->lock, observed_sequence, deadline, 0);
		if (error == ETIMEDOUT)
			break;
	}

	/* Whether the last byte went into the FIFO, and whether the line was reset meanwhile. */
	delivered = 0U;
	if (bt->tx_written >= end)
		delivered = 1U;
	same_line = 0U;
	if (bt->link.generation == observed)
		same_line = 1U;

	spin_unlock_irqrestore(&bt->lock, enabled);

	/* The line was reset under the packet: it may not have reached the controller. */
	if (!same_line)
		return EIO;

	/* The controller held CTS down for too long: it is reset. */
	if (!delivered) {
		self_reset(bt, observed, "send stalled for 2 s", 0);
		return EIO;
	}

	/* Succeeded: every byte of the packet is in the FIFO. */
	return 0;
}

/* Refuses the Intel bootloader's path, which this controller does not have. */
static int
bt_set_bootloader(
	void *context,
	int on)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(on);

	/* Reports a request a Broadcom controller on a UART has no part in. */
	return ENOTSUP;
}

/*
 * Resets the controller for the program (BT_IOC_RESET): powers it off and
 * on with the line at 115200 baud, and queues the RESET notice.  A stopped
 * line starts again; the history of the transport's own resets is kept.
 */
static int
bt_reset(
	void *context)
{
	struct rpi5_bt *bt;
	unsigned long enabled;
	uint32_t line;
	uint32_t modem;
	int error;

	bt = context;

	/*
	 * What the line was doing before the reset: whether the controller let
	 * the host send (its RTS, our CTS) and whether bytes were still waiting
	 * to be sent.
	 */
	line = read_register(bt, UART_LSR);
	modem = read_register(bt, UART_MSR);
	kern_logf("rpi5-bt: before the program's reset: LSR %02x, MSR %02x (CTS %s, send FIFO %s)\n",
	    line,
	    modem,
	    ((modem & UART_MSR_CTS) != 0U) ? "asserted" : "not asserted",
	    ((line & UART_LSR_TX_EMPTY) != 0U) ? "empty" : "holding bytes");

	/*
	 * The bytes that went out and came in since the last reset (the first
	 * SEEN_BYTES of each).
	 */
	log_seen(bt);

	/* Keeps the worker off the line. */
	error = park_worker(bt);
	if (error != 0)
		return error;

	/* The power cycle, as one transition. */
	mutex_lock(&bt->power_lock);

	enabled = spin_lock_irqsave(&bt->lock);

	begin_transition_locked(bt);

	spin_unlock_irqrestore(&bt->lock, enabled);

	/* The power cycle; a controller that does not come up is powered down again. */
	error = power_cycle(bt);
	if (error != 0)
		power_off(bt);

	mutex_unlock(&bt->power_lock);

	/* Tells the program its controller's state is gone, whether or not it came up, before anything of the new line. */
	log_counts(bt, "at the program's reset", 0);
	notice_reset(bt);

	/* The worker finds the new generation and starts afresh. */
	release_worker(bt);

	/* Reports a controller that did not come up (it is stopped). */
	if (error != 0) {
		kern_logf("rpi5-bt: reset failed (%d), controller stopped\n", error);
		return EIO;
	}

	/* Succeeded: the controller runs its ROM at 115200 baud. */
	return 0;
}

/* Wakes the worker: the class has room again for a packet it refused. */
static void
bt_room(
	void *context)
{
	struct rpi5_bt *bt;
	unsigned long enabled;

	bt = context;

	/* room tells the worker to offer its held packet again. */
	enabled = spin_lock_irqsave(&bt->lock);

	bt->room = 1U;
	waitq_wake_all(&bt->worker_wake);

	spin_unlock_irqrestore(&bt->lock, enabled);
}

/*
 * Sets the host's end of the line to a speed the controller was told to
 * change to (BT_IOC_SET_SPEED): waits for the send to drain, sets the
 * divisor, and drops what the line carried meanwhile.  The class checked
 * the speed against speed_max.
 */
static int
bt_set_speed(
	void *context,
	uint32_t baud)
{
	struct rpi5_bt *bt;
	unsigned long enabled;
	uint32_t divisor;
	uint32_t status;
	unsigned waited;
	unsigned running;
	size_t queued;
	int error;

	bt = context;

	/* A speed the divisor cannot come close enough to. */
	error = uart_divisor(bt->clock_hz, baud, &divisor);
	if (error != 0)
		return EINVAL;

	/* Keeps the worker off the line. */
	error = park_worker(bt);
	if (error != 0)
		return error;

	/* The change, as one transition. */
	mutex_lock(&bt->power_lock);

	/* Waits until the send ring and the UART are empty, at most SPEED_DRAIN_MS. */
	waited = 0U;
	for (;;) {
		enabled = spin_lock_irqsave(&bt->lock);

		queued = bt->tx_used;
		running = 0U;
		if (bt->link.mode == DRV_RPI4_BT_MODE_HUNT)
			running = 1U;
		if (bt->link.mode == DRV_RPI4_BT_MODE_SYNC)
			running = 1U;

		spin_unlock_irqrestore(&bt->lock, enabled);

		/* A stopped line has no speed to change. */
		if (!running) {
			error = EIO;
			break;
		}

		/* Drained: the last character has left the UART. */
		status = read_register(bt, UART_LSR);
		if (queued == 0U && (status & UART_LSR_TX_EMPTY) != 0U)
			break;

		/* Still sending after the limit. */
		if (waited >= SPEED_DRAIN_MS) {
			error = EBUSY;
			break;
		}

		/* Looks again a millisecond later. */
		pause_ms(bt, 1U);
		waited++;
	}

	/* The new divisor, with the line quiet before and after it. */
	if (error == 0) {
		enabled = spin_lock_irqsave(&bt->lock);

		begin_transition_locked(bt);

		spin_unlock_irqrestore(&bt->lock, enabled);

		/* The divisor, then the quiet time; a failure stops the line. */
		error = setup_uart(bt, baud);
		if (error == 0)
			finish_quiet(bt);
		if (error != 0)
			power_off(bt);
	}

	mutex_unlock(&bt->power_lock);

	/* The worker finds the new generation, or the unchanged line. */
	release_worker(bt);

	/* Reports a line that could not be changed. */
	if (error != 0)
		return error;

	/* Succeeded: the host's end runs at the new speed. */
	kern_logf("rpi5-bt: line at %u baud\n", baud);
	return 0;
}

/*
 * Serves the line for as long as the kernel runs: parks when an operation
 * asks, starts afresh after every transition, cuts the received bytes into
 * packets for the class, and resets the controller when the line lost
 * step.
 */
static void
worker_loop(
	struct rpi5_bt *bt)
{
	int work;

	/* The first counts are logged ten minutes from now. */
	bt->next_counts_tick = sched_ticks() + kern_ms_to_ticks(COUNTS_PERIOD_MS);
	bt->worker_generation = 0U;
	bt->worker_mode = DRV_RPI4_BT_MODE_OFF;
	bt_hci_assembler_init(
		&bt->assembler,
		bt->assembly,
		sizeof(bt->assembly),
		BT_HCI_ASSEMBLE_H4,
		BT_ACL_DATA_DEFAULT);
	drv_rpi4_bt_hunt_restart(&bt->hunt);

	/* Takes, cuts and delivers, or waits for something to do. */
	for (;;) {
		work = worker_take(bt);
		if (work) {
			worker_process(bt);
			continue;
		}

		/* Nothing to do: waits for the next work. */
		worker_wait(bt);
	}
}

/*
 * Takes the worker's next work under the lock: parks while an operation
 * asks it to, starts afresh when the generation changed, takes the room
 * the class made, and copies a batch of received entries.  Reports
 * whether there is anything to process.
 */
static int
worker_take(
	struct rpi5_bt *bt)
{
	enum drv_rpi4_bt_mode mode;
	unsigned long enabled;
	uint64_t observed;
	uint32_t generation;
	size_t count;
	size_t index;
	int work;

	/* Nothing to do until something below says so. */
	work = 0;
	enabled = spin_lock_irqsave(&bt->lock);

	/* Parks while an operation makes its transition. */
	while (bt->stop_request) {
		bt->parked = 1U;
		waitq_wake_all(&bt->parked_wake);
		observed = waitq_sequence(&bt->worker_wake);
		(void)waitq_sleep(&bt->worker_wake, &bt->lock, observed, 0, 0);
	}

	/* The worker serves the line again. */
	bt->parked = 0U;

	/* A transition since the last look: what was gathered belongs to the old line. */
	generation = bt->link.generation;
	mode = bt->link.mode;
	if (generation != bt->worker_generation || mode != bt->worker_mode)
		worker_restart_locked(bt, generation, mode);

	/* The room the class made lets a held packet be offered again. */
	if (bt->room) {
		bt->room = 0U;
		if (bt->held)
			work = 1;
	}

	/* A new batch once the last one is used up and nothing is held. */
	if (!bt->held && bt->batch_next == bt->batch_have && bt->rx_used != 0U) {
		count = bt->rx_used;
		if (count > RX_BATCH)
			count = RX_BATCH;
		for (index = 0U; index < count; index++)
			bt->batch[index] = bt->rx_ring[(bt->rx_head + index) % RX_RING_ENTRIES];
		bt->rx_head = (bt->rx_head + count) % RX_RING_ENTRIES;
		bt->rx_used -= count;
		bt->batch_have = count;
		bt->batch_next = 0U;
		bt->last_byte_tick = sched_ticks();
		work = 1;

		/* The ring has room again: a masked receiver is let in. */
		if (bt->rx_masked) {
			bt->rx_masked = 0U;
			write_mask_locked(bt, bt->interrupt_mask | UART_IER_RX);
		}
	}

	/* A batch or pending bytes not yet used. */
	if (!bt->held && (bt->batch_next < bt->batch_have || bt->pending_next < bt->pending_have))
		work = 1;

	/* The hunt waits for the answer to the command sent last. */
	if (bt->sent_opcode_known) {
		if (!bt->hunt.opcode_known || bt->hunt.opcode != bt->sent_opcode)
			drv_rpi4_bt_hunt_expect(&bt->hunt, bt->sent_opcode);
	}

	spin_unlock_irqrestore(&bt->lock, enabled);

	/* Reports whether there is anything to process. */
	return work;
}

/*
 * Starts the worker's reassembly afresh for a line at a new generation or
 * mode: the held packet, the hunt and the bytes of the old line go.  The
 * caller holds the lock, before it takes the new line's first batch.
 */
static void
worker_restart_locked(
	struct rpi5_bt *bt,
	uint32_t generation,
	enum drv_rpi4_bt_mode mode)
{
	/* A change of generation drops everything gathered (its packets belong to the old controller). */
	if (generation != bt->worker_generation) {
		bt_hci_assembler_restart(&bt->assembler, BT_HCI_ASSEMBLE_H4, bt->assembler.acl_data_max);
		drv_rpi4_bt_hunt_restart(&bt->hunt);
		bt->held = 0U;
		bt->pending_have = 0U;
		bt->pending_next = 0U;
		bt->batch_have = 0U;
		bt->batch_next = 0U;
	}

	/* The line the worker serves from now on. */
	bt->worker_generation = generation;
	bt->worker_mode = mode;
}

/* Offers a held packet again, then uses the pending bytes and the batch as the line's mode says. */
static void
worker_process(
	struct rpi5_bt *bt)
{
	size_t consumed;
	size_t limit;
	int error;

	/* The ACL limit the program set, which the received ACL headers are checked against. */
	limit = drv_bt_hci_acl_limit(bt->hci);
	bt->assembler.acl_data_max = limit;

	/* A packet the class refused before: offered again, still held if refused again. */
	if (bt->held) {
		error = bt_hci_assembler_feed(&bt->assembler, NULL, 0U, deliver, bt, &consumed);
		if (error == ENOSPC)
			return;
		bt->held = 0U;
	}

	/* A hunting line looks for its anchor; one in step delivers packets. */
	if (bt->worker_mode == DRV_RPI4_BT_MODE_HUNT) {
		error = worker_hunt(bt);
		if (error != 0)
			return;
	}

	/* A line in step cuts the bytes into packets. */
	if (bt->worker_mode == DRV_RPI4_BT_MODE_SYNC) {
		(void)worker_sync(bt);
		return;
	}

	/* A line that is neither drops what it got (a stopped or never powered one receives nothing). */
	if (bt->worker_mode != DRV_RPI4_BT_MODE_HUNT) {
		bt->batch_next = bt->batch_have;
		bt->pending_next = bt->pending_have;
	}
}

/*
 * Hunts through the batch for the answer to fall in step with; a byte
 * with a receive error gives the candidate up.  Returns 0, or ENOSPC when
 * the anchor is held for the class.
 */
static int
worker_hunt(
	struct rpi5_bt *bt)
{
	uint16_t entry;
	size_t length;
	int found;
	int error;

	/* Each entry of the batch until the anchor is found. */
	while (bt->batch_next < bt->batch_have) {
		entry = bt->batch[bt->batch_next];
		bt->batch_next++;

		/* A byte received with an error cannot belong to the answer. */
		if ((entry & ENTRY_ERRORS) != 0U) {
			bt->hunt.dropped++;
			found = drv_rpi4_bt_hunt_give_up(&bt->hunt, &length);
		} else {
			found = drv_rpi4_bt_hunt_byte(&bt->hunt, (uint8_t)(entry & ENTRY_DATA), &length);
		}

		/* The anchor: the line is in step from here on. */
		if (found == DRV_RPI4_BT_HUNT_FOUND) {
			error = worker_anchor(bt, length);
			if (error != 0)
				return error;
			return 0;
		}
	}

	/* Succeeded: the batch is used, still hunting. */
	return 0;
}

/*
 * Falls in step on a found answer: it goes to the class first, the bytes
 * after it are kept to be read in step, and the line becomes SYNC.
 * Returns 0, or ENOSPC when the class refused the answer (it is held).
 */
static int
worker_anchor(
	struct rpi5_bt *bt,
	size_t length)
{
	unsigned long enabled;
	size_t consumed;
	size_t index;
	uint64_t dropped;
	int error;

	/* The answer, loaded whole into the reassembly as though it had been gathered there. */
	error = bt_hci_assembler_load(&bt->assembler, bt->hunt.bytes, length);
	if (error != 0)
		return error;

	/* The bytes after it came after it on the line. */
	bt->pending_have = 0U;
	bt->pending_next = 0U;
	for (index = length; index < bt->hunt.have; index++) {
		bt->pending[bt->pending_have] = bt->hunt.bytes[index];
		bt->pending_have++;
	}

	/* The bytes the hunt passed over, for the log, and a hunt that starts afresh next time. */
	dropped = bt->hunt.dropped;
	drv_rpi4_bt_hunt_restart(&bt->hunt);

	/* The line is in step. */
	enabled = spin_lock_irqsave(&bt->lock);

	drv_rpi_bt_link_anchor(&bt->link);

	spin_unlock_irqrestore(&bt->lock, enabled);

	/* The worker serves the line in step from now on. */
	bt->worker_mode = DRV_RPI4_BT_MODE_SYNC;
	kern_logf("rpi5-bt: in step, %llu bytes passed over\n", (unsigned long long)dropped);

	/* The answer goes to the class, or is held. */
	error = bt_hci_assembler_feed(&bt->assembler, NULL, 0U, deliver, bt, &consumed);
	if (error == ENOSPC) {
		bt->held = 1U;
		return error;
	}

	/* Succeeded: the answer is delivered. */
	return 0;
}

/*
 * Cuts the pending bytes and the batch into packets for the class.  A
 * header no controller sends or a byte received with an error means the
 * line lost step: the controller is reset.  Returns 0, ENOSPC when a
 * packet is held, or EBADMSG after a reset.
 */
static int
worker_sync(
	struct rpi5_bt *bt)
{
	uint8_t bytes[RX_BATCH];
	size_t count;
	size_t consumed;
	uint16_t entry;
	int error;

	/* The bytes kept from the hunt first. */
	if (bt->pending_next < bt->pending_have) {
		error = bt_hci_assembler_feed(
			&bt->assembler,
			bt->pending + bt->pending_next,
			bt->pending_have - bt->pending_next,
			deliver,
			bt,
			&consumed);
		bt->pending_next += consumed;
		if (error == ENOSPC) {
			bt->held = 1U;
			return error;
		}

		/* A header no controller sends: the line lost step. */
		if (error != 0) {
			lost_step(bt, "line out of step (a header no controller sends)");
			return EBADMSG;
		}
	}

	/* The batch, a run of bytes without errors at a time. */
	while (bt->batch_next < bt->batch_have) {
		/* A byte received with an error: the line lost step. */
		entry = bt->batch[bt->batch_next];
		if ((entry & ENTRY_ERRORS) != 0U) {
			lost_step(bt, "line out of step (a receive error)");
			return EBADMSG;
		}

		/* The run up to the next error or the batch's end. */
		count = 0U;
		while (bt->batch_next + count < bt->batch_have) {
			entry = bt->batch[bt->batch_next + count];
			if ((entry & ENTRY_ERRORS) != 0U)
				break;
			bytes[count] = (uint8_t)(entry & ENTRY_DATA);
			count++;
		}

		/* The run into packets, the refused one held. */
		error = bt_hci_assembler_feed(&bt->assembler, bytes, count, deliver, bt, &consumed);
		bt->batch_next += consumed;
		if (error == ENOSPC) {
			bt->held = 1U;
			return error;
		}

		/* A header no controller sends: the line lost step. */
		if (error != 0) {
			lost_step(bt, "line out of step (a header no controller sends)");
			return EBADMSG;
		}
	}

	/* Succeeded: every byte is used. */
	return 0;
}

/*
 * Waits for the next work: received bytes, room, an operation, the end of
 * a gap within a packet, or the time to log the counts.  The wait is
 * decided under the lock the wakers take, so no wakeup is missed.
 */
static void
worker_wait(
	struct rpi5_bt *bt)
{
	unsigned long enabled;
	uint64_t deadline;
	uint64_t gap_deadline;
	uint64_t observed;
	uint64_t now;
	unsigned gathering;
	unsigned idle;
	size_t arrived;
	int error;

	/*
	 * A packet half gathered with no byte coming (and none refused, which
	 * stops the line on purpose) has a deadline.
	 */
	gathering = 0U;
	if (bt->worker_mode == DRV_RPI4_BT_MODE_HUNT && bt->hunt.have != 0U)
		gathering = 1U;
	if (bt->worker_mode == DRV_RPI4_BT_MODE_SYNC && bt->assembler.have != 0U && !bt->held)
		gathering = 1U;
	gap_deadline = bt->last_byte_tick + kern_ms_to_ticks(BYTE_GAP_MS);
	deadline = bt->next_counts_tick;
	if (gathering && gap_deadline < deadline)
		deadline = gap_deadline;

	/* Sleeps when nothing is waiting. */
	error = 0;
	enabled = spin_lock_irqsave(&bt->lock);

	idle = 1U;
	if (bt->stop_request || bt->room)
		idle = 0U;
	if (!bt->held && bt->rx_used != 0U)
		idle = 0U;
	if (bt->link.generation != bt->worker_generation)
		idle = 0U;
	if (idle) {
		observed = waitq_sequence(&bt->worker_wake);
		error = waitq_sleep(&bt->worker_wake, &bt->lock, observed, deadline, 0);
	}

	/* What arrived meanwhile, looked at before the gap is declared. */
	arrived = bt->rx_used;

	spin_unlock_irqrestore(&bt->lock, enabled);

	/* Something woke the worker before its deadline. */
	if (error != ETIMEDOUT)
		return;

	/* Bytes that came just as the deadline passed are read first; the gap ended with them. */
	if (arrived != 0U && !bt->held)
		return;

	/* The gap within a packet ended. */
	now = sched_ticks();
	if (gathering && now >= gap_deadline)
		worker_gap(bt);

	/* The counts, every ten minutes. */
	if (now >= bt->next_counts_tick) {
		log_counts(bt, "periodic", 1);
		bt->next_counts_tick = now + kern_ms_to_ticks(COUNTS_PERIOD_MS);
	}
}

/*
 * Handles a packet cut short (no byte for BYTE_GAP_MS within it): a
 * hunting line gives its candidate up, a line in step lost step.
 */
static void
worker_gap(
	struct rpi5_bt *bt)
{
	size_t length;
	int found;

	/* Hunting: the candidate's first byte goes, and the rest may hold the anchor. */
	if (bt->worker_mode == DRV_RPI4_BT_MODE_HUNT) {
		found = drv_rpi4_bt_hunt_give_up(&bt->hunt, &length);
		if (found == DRV_RPI4_BT_HUNT_FOUND)
			(void)worker_anchor(bt, length);
		bt->last_byte_tick = sched_ticks();
		return;
	}

	/* In step: the controller is reset. */
	lost_step(bt, "line out of step (a packet cut short)");
}

/*
 * Counts a reception the line lost step on as malformed for the class, and
 * resets the controller.  The worker's own.
 */
static void
lost_step(
	struct rpi5_bt *bt,
	const char *why)
{
	struct drv_bt_hci_counts counts;

	/* The class's count (GET_STATS's malformed). */
	counts.malformed = 1U;
	counts.stalls = 0U;
	drv_bt_hci_count(bt->hci, &counts);

	/* The reset, unless an operation's came first. */
	self_reset(bt, bt->worker_generation, why, 1);
}

/*
 * Logs the line's counts: interrupts, bytes received (with errors, and
 * dropped in transitions), bytes sent and the transport's own resets.
 * With only_if_moved, nothing is logged when no interrupt came since the
 * last line.
 */
static void
log_counts(
	struct rpi5_bt *bt,
	const char *when,
	int only_if_moved)
{
	unsigned long enabled;
	uint64_t interrupts;
	uint64_t received;
	uint64_t errors;
	uint64_t sent;
	uint64_t dropped;
	uint64_t resets;
	unsigned moved;

	/* The counts now, and whether an interrupt came since the last line. */
	enabled = spin_lock_irqsave(&bt->lock);

	interrupts = bt->interrupts;
	received = bt->rx_bytes;
	errors = bt->rx_errors;
	sent = bt->tx_bytes;
	dropped = bt->quiet_dropped;
	resets = bt->self_resets;
	moved = 0U;
	if (interrupts != bt->counts_logged)
		moved = 1U;
	bt->counts_logged = interrupts;

	spin_unlock_irqrestore(&bt->lock, enabled);

	/* A quiet line is not logged again. */
	if (only_if_moved && !moved)
		return;

	/* One line, compared at the start and the end of a long test (ws215 p006). */
	kern_logf(
		"rpi5-bt: %s: %llu interrupts, %llu bytes in (%llu with errors, %llu in transitions), %llu out, %llu own resets\n",
		when,
		(unsigned long long)interrupts,
		(unsigned long long)received,
		(unsigned long long)errors,
		(unsigned long long)dropped,
		(unsigned long long)sent,
		(unsigned long long)resets);
}

/* Logs, in hexadecimal, the first bytes that went out and came in since the last reset. */
static void
log_seen(
	struct rpi5_bt *bt)
{
	unsigned long enabled;
	uint8_t seen_in[SEEN_BYTES];
	uint8_t seen_in_errors[SEEN_BYTES];
	uint8_t seen_out[SEEN_BYTES];
	unsigned in_count;
	unsigned out_count;
	char in_text[SEEN_BYTES * 3U + 1U];
	char error_text[SEEN_BYTES * 2U + 1U];
	char out_text[SEEN_BYTES * 3U + 1U];
	unsigned index;
	unsigned shown;
	unsigned errors;

	/* A copy of the bytes, taken under the lock. */
	enabled = spin_lock_irqsave(&bt->lock);

	in_count = bt->seen_in_count;
	out_count = bt->seen_out_count;
	kern_memcpy(seen_in, bt->seen_in, sizeof(seen_in));
	kern_memcpy(seen_in_errors, bt->seen_in_errors, sizeof(seen_in_errors));
	kern_memcpy(seen_out, bt->seen_out, sizeof(seen_out));

	spin_unlock_irqrestore(&bt->lock, enabled);

	/* The bytes out, as text. */
	out_text[0] = '\0';
	shown = out_count;
	if (shown > SEEN_BYTES)
		shown = SEEN_BYTES;
	for (index = 0; index < shown; index++)
		(void)kern_snprintf(out_text + index * 3U, sizeof(out_text) - index * 3U, " %02x", seen_out[index]);

	/* The bytes in, as text. */
	in_text[0] = '\0';
	shown = in_count;
	if (shown > SEEN_BYTES)
		shown = SEEN_BYTES;
	for (index = 0; index < shown; index++)
		(void)kern_snprintf(in_text + index * 3U, sizeof(in_text) - index * 3U, " %02x", seen_in[index]);

	/* The error bits of each byte in, as a hexadecimal digit (1 framing, 2 parity, 4 break, 8 overrun), and whether any is set. */
	error_text[0] = '\0';
	errors = 0U;
	for (index = 0; index < shown; index++) {
		(void)kern_snprintf(error_text + index * 2U, sizeof(error_text) - index * 2U, " %x", (unsigned)seen_in_errors[index]);
		if (seen_in_errors[index] != 0U)
			errors = 1U;
	}

	/* One line each; the errors' line only when a byte had one. */
	kern_logf("rpi5-bt: since the last reset %u bytes out:%s\n", out_count, out_text);
	kern_logf("rpi5-bt: since the last reset %u bytes in:%s\n", in_count, in_text);
	if (errors)
		kern_logf("rpi5-bt: their errors (1 framing, 2 parity, 4 break, 8 overrun):%s\n", error_text);
}

/*
 * Hands one packet the reassembly put together to the class: events and
 * ACL packets are queued (ENOSPC when the class has no room, and the packet
 * is held); SCO packets are dropped until ws215-p008 carries them.
 */
static int
deliver(
	void *context,
	uint8_t type,
	const uint8_t *packet,
	size_t length)
{
	struct drv_bt_hci_counts counts;
	struct rpi5_bt *bt;
	int error;

	bt = context;

	/* No SCO is carried on this line yet. */
	if (type == BT_PACKET_SCO)
		return 0;

	/* The class queues it, or has no room now. */
	error = drv_bt_hci_input(bt->hci, type, packet, length);
	if (error == ENOSPC) {
		counts.malformed = 0U;
		counts.stalls = 1U;
		drv_bt_hci_count(bt->hci, &counts);
		return ENOSPC;
	}

	/* Succeeded: the packet is the class's (or dropped with nobody to read it). */
	return 0;
}

/* Reads one UART register, always as a 32-bit word. */
static uint32_t
read_register(
	struct rpi5_bt *bt,
	uint32_t offset)
{
	uint32_t value;

	/* The word at the register's offset. */
	value = kern_mmio_read32((const volatile uint8_t *)bt->registers + offset);

	/* Succeeded: the register's value. */
	return value;
}

/* Writes one UART register, always as a 32-bit word. */
static void
write_register(
	struct rpi5_bt *bt,
	uint32_t offset,
	uint32_t value)
{
	/* The word at the register's offset. */
	kern_mmio_write32((volatile uint8_t *)bt->registers + offset, value);
}

/* Sleeps for the given milliseconds; nothing ends the sleep early. */
static void
pause_ms(
	struct rpi5_bt *bt,
	unsigned milliseconds)
{
	unsigned long enabled;
	uint64_t deadline;
	uint64_t observed;
	int error;

	/* The scheduler tick at which the sleep ends. */
	deadline = sched_ticks() + kern_ms_to_ticks(milliseconds);

	/* Sleeps on a queue nobody wakes until the deadline. */
	enabled = spin_lock_irqsave(&bt->lock);

	for (;;) {
		observed = waitq_sequence(&bt->pause_wake);
		error = waitq_sleep(&bt->pause_wake, &bt->lock, observed, deadline, 0);
		if (error == ETIMEDOUT)
			break;
	}

	spin_unlock_irqrestore(&bt->lock, enabled);
}

/* Reports the scheduler's time in milliseconds, for the history of resets. */
static uint64_t
now_ms(void)
{
	uint64_t ticks;
	uint64_t milliseconds;

	/* The ticks since boot. */
	ticks = sched_ticks();

	/* Succeeded: the same time in milliseconds. */
	milliseconds = kern_ticks_to_ms(ticks);
	return milliseconds;
}

/* Drives BT_ON to power the controller on or off, by the line's polarity. */
static int
set_power(
	struct rpi5_bt *bt,
	int on)
{
	int level;
	int error;

	/* The level that means on: high unless the line is active low. */
	level = 0;
	if (on != 0 && !bt->power_active_low)
		level = 1;
	if (on == 0 && bt->power_active_low)
		level = 1;

	/* Drives the pin. */
	error = drv_rpi5_gio_set_output(&bt->gio, bt->power_pin, level);
	if (error != 0)
		return error;

	/* Succeeded: the controller's power follows. */
	return 0;
}

/* Turns the line status of a received byte into the ring entry's error bits. */
static uint32_t
entry_errors(
	uint32_t status)
{
	uint32_t errors;

	/* Each error the status reported for the byte. */
	errors = 0U;
	if ((status & UART_LSR_FRAMING) != 0U)
		errors |= ENTRY_FRAMING;
	if ((status & UART_LSR_PARITY) != 0U)
		errors |= ENTRY_PARITY;
	if ((status & UART_LSR_BREAK) != 0U)
		errors |= ENTRY_BREAK;
	if ((status & UART_LSR_OVERRUN) != 0U)
		errors |= ENTRY_OVERRUN;

	/* Succeeded: the entry's error bits. */
	return errors;
}

/*
 * Works out the divisor of a speed: the clock over sixteen times the
 * speed, rounded; EINVAL when none comes within 1.5 percent.
 */
static int
uart_divisor(
	uint32_t clock_hz,
	uint32_t baud,
	uint32_t *divisor)
{
	uint64_t rounded;
	uint64_t actual;
	uint64_t difference;

	/* A rate and a clock to divide. */
	if (clock_hz == 0U || baud == 0U)
		return EINVAL;

	/* The nearest divisor. */
	rounded = ((uint64_t)clock_hz + (uint64_t)baud * UART_OVERSAMPLE / 2U) / ((uint64_t)baud * UART_OVERSAMPLE);
	if (rounded == 0U || rounded > UART_DIVISOR_MAX)
		return EINVAL;

	/* The speed it gives, and how far that is from the one asked for. */
	actual = (uint64_t)clock_hz / (rounded * UART_OVERSAMPLE);
	difference = baud - actual;
	if (actual > baud)
		difference = actual - baud;

	/* Refuses a speed too far from the one asked for. */
	if (difference * 1000U > (uint64_t)baud * UART_ERROR_PERMILLE)
		return EINVAL;

	/* Succeeded: the divisor. */
	*divisor = (uint32_t)rounded;
	return 0;
}

/* The fastest speed the UART's clock and the tree's limit both allow. */
static uint32_t
uart_speed_limit(
	uint32_t clock_hz,
	uint32_t tree_max)
{
	uint32_t limit;

	/* The clock's limit: one bit every sixteen cycles. */
	limit = clock_hz / UART_OVERSAMPLE;

	/* The tree's, when it gives a lower one. */
	if (tree_max != 0U && tree_max < limit)
		limit = tree_max;

	/* Succeeded: the speed. */
	return limit;
}
