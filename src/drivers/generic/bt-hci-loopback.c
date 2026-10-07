/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The test kernel's loopback Bluetooth controller (ws143-p002): an HCI
 * class controller with no hardware, so that /dev/btN can be tried in QEMU,
 * which has no Bluetooth controller.  Test builds only
 * (CONFIG_BT_TEST_LOOPBACK, as the loopback security key and card of
 * ws161 are).
 *
 * It answers as far as the class's tests need:
 *
 *   a command 0xFC01   Command Complete after an event, an ACL packet, an
 *                      event and an ACL packet, in that order (the two
 *                      queues' order)
 *   a command 0xFC02   Command Complete, then N vendor events (0xFF) of 255
 *                      bytes, N the two parameter bytes (little-endian),
 *                      each carrying its index: more than the events' queue
 *                      holds (the backpressure and the notices' reserve)
 *   a command 0xFC03   Command Complete; LOOPBACK_WITHDRAW_MS (or the
 *                      milliseconds of its two parameter bytes, at most
 *                      LOOPBACK_DELAY_MOST, ws143-p003) later the
 *                      controller is withdrawn and released (a detach), and
 *                      LOOPBACK_RETURN_MS after that it is registered again
 *   0x1001, 0x1009     Read Local Version Information, Read BD_ADDR
 *   0x1002, 0x1003     Read Local Supported Commands (Inquiry, LE's
 *                      event mask and scan, P-256 and DHKey), Read Local
 *                      Supported Features (LE), ws143-p003
 *   0x1005             Read Buffer Size (ACL 1021 bytes, 8 packets)
 *   0x0401             Inquiry: Command Status, then an Extended Inquiry
 *                      Result ("Loopback Keyboard", class 0x002540), an
 *                      Inquiry Result with RSSI (class 0x002580) and
 *                      Inquiry Complete (ws143-p003)
 *   0x200C on          LE Set Scan Enable: Command Complete, then two
 *                      advertising reports: a public address with
 *                      "Loopback Mouse" and appearance 0x03C2, a random one
 *                      without a name (ws143-p003)
 *   any other command  Command Complete with status 0
 *   an ACL packet      the same packet back
 *
 * The reset (BT_IOC_RESET) drops what is not delivered yet and queues the
 * reset's notice.  A worker thread delivers everything; a packet the class
 * has no room for waits for the room call, so nothing is dropped.
 */

#include <drivers/generic/bt-hci.h>

#include <kern/clock.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <uapi/errno.h>

/* Marks a parameter a function takes for its signature's sake. */
#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(parameter) ((void)(parameter))
#endif

/* The test commands (OGF 0x3F, vendor-specific). */
#define LOOPBACK_OP_ORDER		0xfc01U
#define LOOPBACK_OP_FLOOD		0xfc02U
#define LOOPBACK_OP_WITHDRAW		0xfc03U

/* The standard commands it answers with their own return parameters. */
#define LOOPBACK_OP_INQUIRY		0x0401U
#define LOOPBACK_OP_LOCAL_VERSION	0x1001U
#define LOOPBACK_OP_COMMANDS		0x1002U
#define LOOPBACK_OP_FEATURES		0x1003U
#define LOOPBACK_OP_BUFFER_SIZE		0x1005U
#define LOOPBACK_OP_BD_ADDR		0x1009U
#define LOOPBACK_OP_LE_SCAN_ENABLE	0x200cU

/* The events it sends: Inquiry Complete, Command Complete and Status, the inquiry results, LE meta, and a vendor event. */
#define LOOPBACK_EVENT_INQUIRY_COMPLETE	0x01U
#define LOOPBACK_EVENT_COMPLETE		0x0eU
#define LOOPBACK_EVENT_STATUS		0x0fU
#define LOOPBACK_EVENT_INQUIRY_RSSI	0x22U
#define LOOPBACK_EVENT_EXTENDED_INQUIRY	0x2fU
#define LOOPBACK_EVENT_LE_META		0x3eU
#define LOOPBACK_EVENT_VENDOR		0xffU

/* The most return parameters a Command Complete carries (its length byte holds 3 more). */
#define LOOPBACK_RETURN_MOST		252U

/* The longest delay 0xFC03 may ask for. */
#define LOOPBACK_DELAY_MOST		10000U

/* A flood's events' parameters, and the most a flood may ask for. */
#define LOOPBACK_FLOOD_PARAMETERS	255U
#define LOOPBACK_FLOOD_MOST		4096U

/* The packets waiting to be delivered (records of bt-hci-proto.c's ring). */
#define LOOPBACK_RING			(64U * 1024U)

/* How long after 0xFC03 the controller goes, and how long after that it comes back. */
#define LOOPBACK_WITHDRAW_MS		300U
#define LOOPBACK_RETURN_MS		500U

/* How often the worker looks at a withdrawal that is not due yet, delivering meanwhile. */
#define LOOPBACK_POLL_MS		10U

/*
 * The loopback controller's state, one for the kernel's life.
 *
 * lock guards the waiting packets (ring), the flood still to send, the
 * worker's requests (work, withdraw) and hci.  deliver is held by the
 * worker while it hands one packet to the class, and by the reset while it
 * drops what waits and queues the notice, so that no packet from before the
 * reset is delivered after its notice.  packet is the worker's own copy of
 * the packet it delivers.
 */
struct loopback_controller {
	struct spinlock lock;
	struct mutex deliver;
	struct bt_hci_ring ring;
	uint8_t ring_bytes[LOOPBACK_RING];
	unsigned flood_remaining;
	unsigned flood_next;
	unsigned work;
	unsigned withdraw;
	uint64_t withdraw_due;
	unsigned stall_counted;
	struct drv_bt_hci *hci;
	struct thread *worker;
	uint8_t packet[BT_ACL_PACKET_MAX];
};

/* The one loopback controller, zero until it is registered. */
static struct loopback_controller loopback;

static int loopback_publish(void);
static void loopback_worker(void *argument);
static int loopback_deliver_next(void);
static void loopback_withdraw_and_return(void);
static void loopback_wake(void);
static int loopback_queue(uint8_t type, const uint8_t *body, size_t length);
static int loopback_complete(uint16_t opcode, const uint8_t *parameters, size_t count);
static int loopback_order(uint16_t opcode);
static int loopback_send(void *context, const uint8_t *packet, size_t length);
static int loopback_set_bootloader(void *context, int on);
static int loopback_reset(void *context);
static void loopback_room(void *context);
static void loopback_sleep_ms(unsigned milliseconds);
static int loopback_status(uint16_t opcode);
static int loopback_inquiry(uint16_t opcode);
static int loopback_advertise(uint16_t opcode);

/* What the loopback controller does for the HCI class. */
static const struct drv_bt_hci_ops loopback_ops = {
	.send = loopback_send,
	.set_bootloader = loopback_set_bootloader,
	.reset = loopback_reset,
	.room = loopback_room
};

/*
 * Publishes the loopback controller and starts its worker (test builds:
 * called once at boot).  Returns 0 or the error of publishing it.
 */
int
drv_bt_hci_loopback_register(
	void)
{
	int error;

	/* The locks and the empty queue of waiting packets. */
	spin_init(&loopback.lock, LOCK_RANK_DEVICE, "bt-loopback");
	error = mutex_init(&loopback.deliver, LOCK_RANK_DEVICE, "bt-loopback deliver");
	if (error != 0)
		return error;
	bt_hci_ring_init(&loopback.ring, loopback.ring_bytes, LOOPBACK_RING, 0U);

	/* The node. */
	error = loopback_publish();
	if (error != 0)
		return error;

	/* The worker that delivers the packets. */
	error = kthread_create(loopback_worker, NULL, SCHED_PRIORITY_DEFAULT, &loopback.worker);
	if (error != 0)
		return error;
	thread_start(loopback.worker);

	/* Succeeded: the test controller is there. */
	kern_logf("bt-loopback: test controller published\n");
	return 0;
}

/* Registers the controller with the class (at boot, and again after a test's withdrawal). */
static int
loopback_publish(
	void)
{
	struct drv_bt_hci_description description;
	struct drv_bt_hci *hci;
	unsigned long irq;
	int error;

	/* What the controller is: a test device of the pid.codes test range. */
	kern_memset(&description, 0, sizeof(description));
	description.vendor = 0x1209U;
	description.product = 0xb7e5U;
	description.version = 0x0100U;
	description.bus = BT_BUS_USB;
	description.name = "Loopback Bluetooth controller (test)";
	description.physical_path = "loopback0";

	/* The class publishes btN. */
	error = drv_bt_hci_register(&description, &loopback_ops, &loopback, &hci);
	if (error != 0)
		return error;

	/* The worker delivers to it from now on. */
	irq = spin_lock_irqsave(&loopback.lock);

	loopback.hci = hci;

	spin_unlock_irqrestore(&loopback.lock, irq);

	/* Succeeded: published. */
	return 0;
}

/* Delivers the waiting packets to the class, and does the test's withdrawal, for the kernel's life. */
static void
loopback_worker(
	void *argument)
{
	unsigned long irq;
	unsigned withdraw;
	unsigned due;
	uint64_t now;
	int delivered;

	UNUSED_PARAMETER(argument);

	/* Each round: the packets, then a withdrawal asked for (after its command's answer), else a sleep until a request. */
	for (;;) {
		irq = spin_lock_irqsave(&loopback.lock);

		loopback.work = 0U;

		spin_unlock_irqrestore(&loopback.lock, irq);

		/* Every packet the class takes now. */
		delivered = loopback_deliver_next();
		while (delivered)
			delivered = loopback_deliver_next();

		/* A withdrawal asked for, and whether it is due. */
		irq = spin_lock_irqsave(&loopback.lock);

		withdraw = loopback.withdraw;
		due = 0U;
		if (withdraw) {
			now = sched_ticks();
			if (now >= loopback.withdraw_due) {
				due = 1U;
				loopback.withdraw = 0U;
			}
		}

		spin_unlock_irqrestore(&loopback.lock, irq);

		/* The test's detach once it is due. */
		if (due) {
			loopback_withdraw_and_return();
			continue;
		}

		/* Not due yet: the packets keep going meanwhile, looked at every LOOPBACK_POLL_MS. */
		if (withdraw) {
			loopback_sleep_ms(LOOPBACK_POLL_MS);
			continue;
		}

		/* Nothing more until a request (a send, the room call, a withdrawal; one made just before is kept). */
		kern_thread_block();
	}
}

/*
 * Hands the next waiting packet to the class.  Returns nonzero when one was
 * delivered (or dropped as the class refused its type), 0 when nothing
 * waits or the class has no room for it now.
 */
static int
loopback_deliver_next(
	void)
{
	struct drv_bt_hci_counts counts;
	struct drv_bt_hci *hci;
	unsigned long irq;
	unsigned from_flood;
	uint32_t sequence;
	size_t length;
	uint8_t type;
	int error;

	/* No reset drops what waits while one packet is being delivered. */
	mutex_lock(&loopback.deliver);

	/* The next packet: from the queue, else the flood's next event. */
	from_flood = 0U;
	length = 0U;
	irq = spin_lock_irqsave(&loopback.lock);

	hci = loopback.hci;
	if (loopback.ring.count != 0U) {
		(void)bt_hci_ring_peek(&loopback.ring, &type, &sequence, &length);
		length = bt_hci_ring_copy(&loopback.ring, loopback.packet);
	} else if (loopback.flood_remaining != 0U) {
		loopback.packet[0] = BT_PACKET_EVENT;
		loopback.packet[1] = LOOPBACK_EVENT_VENDOR;
		loopback.packet[2] = (uint8_t)LOOPBACK_FLOOD_PARAMETERS;
		kern_memset(loopback.packet + 3, 0x5a, LOOPBACK_FLOOD_PARAMETERS);
		loopback.packet[3] = (uint8_t)(loopback.flood_next & 0xffU);
		loopback.packet[4] = (uint8_t)(loopback.flood_next >> 8);
		length = 3U + LOOPBACK_FLOOD_PARAMETERS;
		from_flood = 1U;
	}

	spin_unlock_irqrestore(&loopback.lock, irq);

	/* Nothing waits, or no controller is published. */
	if (length == 0U || hci == NULL) {
		mutex_unlock(&loopback.deliver);
		return 0;
	}

	/* The class queues it, or has no room now. */
	error = drv_bt_hci_input(hci, loopback.packet[0], loopback.packet + 1, length - 1U);
	if (error == ENOSPC) {
		/* Counted once a refusal, as the USB transport counts its stalls. */
		irq = spin_lock_irqsave(&loopback.lock);

		counts.stalls = 0U;
		if (!loopback.stall_counted)
			counts.stalls = 1U;
		loopback.stall_counted = 1U;

		spin_unlock_irqrestore(&loopback.lock, irq);

		/* The class's counts. */
		counts.malformed = 0U;
		drv_bt_hci_count(hci, &counts);
		mutex_unlock(&loopback.deliver);
		return 0;
	}

	/* Taken (or refused for its type): it waits no more. */
	irq = spin_lock_irqsave(&loopback.lock);

	loopback.stall_counted = 0U;
	if (from_flood) {
		loopback.flood_remaining--;
		loopback.flood_next++;
	} else {
		bt_hci_ring_pop(&loopback.ring);
	}

	spin_unlock_irqrestore(&loopback.lock, irq);

	/* Succeeded: one packet is done. */
	mutex_unlock(&loopback.deliver);
	return 1;
}

/*
 * Does the test's detach: withdraws and releases the controller as a USB
 * transport's detach does, drops what waits, and registers it again after
 * a while (the class gives it the lowest free number, its old one).
 */
static void
loopback_withdraw_and_return(
	void)
{
	struct drv_bt_hci *hci;
	unsigned long irq;
	int error;

	/* The controller the class has, which no delivery uses from now on. */
	irq = spin_lock_irqsave(&loopback.lock);

	hci = loopback.hci;
	loopback.hci = NULL;

	spin_unlock_irqrestore(&loopback.lock, irq);

	/* No delivery is under way with it. */
	mutex_lock(&loopback.deliver);
	mutex_unlock(&loopback.deliver);

	/* The class's detach: no operation after the withdrawal, then the node goes. */
	if (hci != NULL) {
		drv_bt_hci_withdraw(hci);
		drv_bt_hci_release(hci);
		kern_logf("bt-loopback: test controller withdrawn\n");
	}

	/* What waited went with it. */
	irq = spin_lock_irqsave(&loopback.lock);

	bt_hci_ring_clear(&loopback.ring);
	loopback.flood_remaining = 0U;
	loopback.stall_counted = 0U;

	spin_unlock_irqrestore(&loopback.lock, irq);

	/* Some time without a controller, then it comes back. */
	loopback_sleep_ms(LOOPBACK_RETURN_MS);
	error = loopback_publish();
	if (error != 0) {
		kern_logf("bt-loopback: test controller not published again (%d)\n", error);
		return;
	}

	/* Logged for the test. */
	kern_logf("bt-loopback: test controller published again\n");
}

/* Wakes the worker for a request (it does not sleep). */
static void
loopback_wake(
	void)
{
	struct thread *worker;
	unsigned long irq;

	/* The request, and the worker to wake. */
	irq = spin_lock_irqsave(&loopback.lock);

	loopback.work = 1U;
	worker = loopback.worker;

	spin_unlock_irqrestore(&loopback.lock, irq);

	/* A worker not started yet finds the request when it starts. */
	if (worker != NULL)
		kern_thread_wakeup(worker);
}

/* Queues one packet (header first, without the type's byte) to be delivered; ENOSPC when the queue is full. */
static int
loopback_queue(
	uint8_t type,
	const uint8_t *body,
	size_t length)
{
	unsigned long irq;
	int error;

	/* At the end of the waiting packets. */
	irq = spin_lock_irqsave(&loopback.lock);

	error = bt_hci_ring_push(&loopback.ring, type, 0U, body, length, 0);

	spin_unlock_irqrestore(&loopback.lock, irq);

	/* Reports a full queue. */
	if (error != 0)
		return error;

	/* Succeeded: queued. */
	return 0;
}

/* Queues a Command Complete event for an opcode with its return parameters (the status first). */
static int
loopback_complete(
	uint16_t opcode,
	const uint8_t *parameters,
	size_t count)
{
	uint8_t event[2U + 3U + LOOPBACK_RETURN_MOST];
	int error;

	/* The event: its code, its length, one credit, the opcode and the return parameters. */
	if (count > LOOPBACK_RETURN_MOST)
		return EINVAL;
	event[0] = LOOPBACK_EVENT_COMPLETE;
	event[1] = (uint8_t)(3U + count);
	event[2] = 1U;
	event[3] = (uint8_t)(opcode & 0xffU);
	event[4] = (uint8_t)(opcode >> 8);
	kern_memcpy(event + 5, parameters, count);

	/* Queued. */
	error = loopback_queue(BT_PACKET_EVENT, event, 5U + count);
	if (error != 0)
		return error;

	/* Succeeded: the answer waits to be delivered. */
	return 0;
}

/*
 * Queues the order test's packets: an event, an ACL packet, an event and
 * an ACL packet, each carrying its place (1 to 4) in its last byte, then
 * the command's answer.
 */
static int
loopback_order(
	uint16_t opcode)
{
	static const uint8_t status_ok[1] = { 0x00U };
	uint8_t event[3];
	uint8_t acl[5];
	int error;

	/* The first event: a vendor event of one byte, 1. */
	event[0] = LOOPBACK_EVENT_VENDOR;
	event[1] = 1U;
	event[2] = 1U;
	error = loopback_queue(BT_PACKET_EVENT, event, sizeof(event));
	if (error != 0)
		return error;

	/* The first ACL packet: handle 0x001, one byte of data, 2. */
	acl[0] = 0x01U;
	acl[1] = 0x20U;
	acl[2] = 1U;
	acl[3] = 0U;
	acl[4] = 2U;
	error = loopback_queue(BT_PACKET_ACL, acl, sizeof(acl));
	if (error != 0)
		return error;

	/* The second event, 3. */
	event[2] = 3U;
	error = loopback_queue(BT_PACKET_EVENT, event, sizeof(event));
	if (error != 0)
		return error;

	/* The second ACL packet, 4. */
	acl[4] = 4U;
	error = loopback_queue(BT_PACKET_ACL, acl, sizeof(acl));
	if (error != 0)
		return error;

	/* The answer last. */
	error = loopback_complete(opcode, status_ok, sizeof(status_ok));
	if (error != 0)
		return error;

	/* Succeeded: all five wait for the worker. */
	return 0;
}

/* Takes one checked H4 packet (the class's operation): answers a command, sends an ACL packet back. */
static int
loopback_send(
	void *context,
	const uint8_t *packet,
	size_t length)
{
	static const uint8_t status_ok[1] = { 0x00U };
	static const uint8_t local_version[9] = { 0x00U, 0x0cU, 0x00U, 0x01U, 0x0cU, 0xffU, 0xffU, 0x01U, 0x00U };
	static const uint8_t bd_addr[7] = { 0x00U, 0x55U, 0x44U, 0x33U, 0x22U, 0x11U, 0x00U };
	static const uint8_t features[9] = { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x40U, 0x00U, 0x00U, 0x00U };
	static const uint8_t buffer_size[8] = { 0x00U, 0xfdU, 0x03U, 0x00U, 0x08U, 0x00U, 0x00U, 0x00U };
	uint8_t commands[1U + 64U];
	unsigned long irq;
	uint16_t opcode;
	unsigned count;
	unsigned delay;
	int error;

	UNUSED_PARAMETER(context);

	/* An ACL packet comes back as it went; a command has its opcode. */
	error = 0;
	opcode = 0U;
	if (packet[0] == BT_PACKET_ACL) {
		error = loopback_queue(BT_PACKET_ACL, packet + 1, length - 1U);
	} else {
		opcode = (uint16_t)(packet[1] | (packet[2] << 8));
	}

	/* Each command the tests use (none for an ACL packet). */
	switch (opcode) {
	case 0U:
		/* The ACL packet's echo is queued already. */
		break;
	case LOOPBACK_OP_ORDER:
		/* An event, an ACL packet, an event and an ACL packet, then the answer. */
		error = loopback_order(opcode);
		break;
	case LOOPBACK_OP_FLOOD:
		/* The answer first, then the events the parameters ask for. */
		if (length < 6U)
			return EINVAL;
		count = (unsigned)(packet[4] | (packet[5] << 8));
		if (count > LOOPBACK_FLOOD_MOST)
			return EINVAL;
		error = loopback_complete(opcode, status_ok, sizeof(status_ok));
		if (error == 0) {
			/* The flood, counted from 0. */
			irq = spin_lock_irqsave(&loopback.lock);

			loopback.flood_remaining = count;
			loopback.flood_next = 0U;

			spin_unlock_irqrestore(&loopback.lock, irq);
		}

		/* The flood is under way, or the answer did not fit. */
		break;
	case LOOPBACK_OP_WITHDRAW:
		/* The delay: the two parameter bytes when given, else LOOPBACK_WITHDRAW_MS. */
		delay = LOOPBACK_WITHDRAW_MS;
		if (length >= 6U)
			delay = (unsigned)(packet[4] | (packet[5] << 8));
		if (delay > LOOPBACK_DELAY_MOST)
			return EINVAL;

		/* The answer, then the withdrawal after the delay (by the worker, which may take the operations' lock). */
		error = loopback_complete(opcode, status_ok, sizeof(status_ok));
		if (error == 0) {
			/* Asked of the worker. */
			irq = spin_lock_irqsave(&loopback.lock);

			loopback.withdraw = 1U;
			loopback.withdraw_due = sched_ticks() + kern_ms_to_ticks(delay);

			spin_unlock_irqrestore(&loopback.lock, irq);
		}

		/* The withdrawal is asked for, or the answer did not fit. */
		break;
	case LOOPBACK_OP_LOCAL_VERSION:
		/* Bluetooth 5.3, the test's manufacturer 0xFFFF. */
		error = loopback_complete(opcode, local_version, sizeof(local_version));
		break;
	case LOOPBACK_OP_BD_ADDR:
		/* 00:11:22:33:44:55. */
		error = loopback_complete(opcode, bd_addr, sizeof(bd_addr));
		break;
	case LOOPBACK_OP_COMMANDS:
		/* Inquiry (octet 0), LE Set Event Mask (25), LE Set Scan Parameters and Enable (26), P-256 and DHKey (34). */
		kern_memset(commands, 0, sizeof(commands));
		commands[1U + 0U] = 0x03U;
		commands[1U + 25U] = 0x01U;
		commands[1U + 26U] = 0x0cU;
		commands[1U + 34U] = 0x06U;
		error = loopback_complete(opcode, commands, sizeof(commands));
		break;
	case LOOPBACK_OP_FEATURES:
		/* LE Supported (Controller): byte 4, bit 6. */
		error = loopback_complete(opcode, features, sizeof(features));
		break;
	case LOOPBACK_OP_BUFFER_SIZE:
		/* ACL 1021 bytes, no SCO, 8 ACL packets. */
		error = loopback_complete(opcode, buffer_size, sizeof(buffer_size));
		break;
	case LOOPBACK_OP_INQUIRY:
		/* Command Status, the two results and the inquiry's end. */
		error = loopback_inquiry(opcode);
		break;
	case LOOPBACK_OP_LE_SCAN_ENABLE:
		/* On: the answer and two reports; off: the answer alone. */
		if (length >= 5U && packet[4] == 1U) {
			error = loopback_advertise(opcode);
		} else {
			error = loopback_complete(opcode, status_ok, sizeof(status_ok));
		}

		break;
	default:
		/* Done, nothing to say. */
		error = loopback_complete(opcode, status_ok, sizeof(status_ok));
		break;
	}

	/* Reports a full queue of waiting packets. */
	if (error != 0)
		return error;

	/* Succeeded: the answers wait for the worker. */
	loopback_wake();
	return 0;
}

/* Turns the bootloader's path on or off (nothing changes for the loopback controller). */
static int
loopback_set_bootloader(
	void *context,
	int on)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(on);

	/* Succeeded: the class keeps the flag. */
	return 0;
}

/*
 * Resets the controller (the class's operation): what waits is dropped and
 * the reset's notice is queued, both while no packet is being delivered.
 */
static int
loopback_reset(
	void *context)
{
	struct drv_bt_hci *hci;
	unsigned long irq;

	UNUSED_PARAMETER(context);

	/* No packet is being delivered meanwhile. */
	mutex_lock(&loopback.deliver);

	/* Nothing from before the reset is delivered. */
	irq = spin_lock_irqsave(&loopback.lock);

	bt_hci_ring_clear(&loopback.ring);
	loopback.flood_remaining = 0U;
	loopback.stall_counted = 0U;
	hci = loopback.hci;

	spin_unlock_irqrestore(&loopback.lock, irq);

	/* The notice, after everything from before. */
	if (hci != NULL)
		drv_bt_hci_notice(hci, BT_PACKET_NOTICE_RESET);

	/* Succeeded: reset. */
	mutex_unlock(&loopback.deliver);
	return 0;
}

/* Tells the worker that a read has made room (the class's operation; it does not sleep). */
static void
loopback_room(
	void *context)
{
	UNUSED_PARAMETER(context);

	/* The worker tries the waiting packet again. */
	loopback_wake();
}

/* Queues a Command Status event with status 0 for an opcode (a command whose work goes on). */
static int
loopback_status(
	uint16_t opcode)
{
	uint8_t event[6];
	int error;

	/* The event: its code, its length 4, the status, one credit and the opcode. */
	event[0] = LOOPBACK_EVENT_STATUS;
	event[1] = 4U;
	event[2] = 0x00U;
	event[3] = 1U;
	event[4] = (uint8_t)(opcode & 0xffU);
	event[5] = (uint8_t)(opcode >> 8);

	/* Queued. */
	error = loopback_queue(BT_PACKET_EVENT, event, sizeof(event));
	if (error != 0)
		return error;

	/* Succeeded: the status waits to be delivered. */
	return 0;
}

/*
 * Queues an inquiry's answer and results: Command Status, an Extended
 * Inquiry Result (address 0A:0B:0C:0D:0E:01, class 0x002540, RSSI -40,
 * the complete name "Loopback Keyboard"), an Inquiry Result with RSSI
 * (0A:0B:0C:0D:0E:02, class 0x002580, RSSI -60) and Inquiry Complete.
 */
static int
loopback_inquiry(
	uint16_t opcode)
{
	static const char name[] = "Loopback Keyboard";
	uint8_t extended[2U + 255U];
	uint8_t plain[2U + 15U];
	uint8_t complete[3];
	size_t name_length;
	int error;

	/* The command goes on. */
	error = loopback_status(opcode);
	if (error != 0)
		return error;

	/* The extended result: one response and 240 bytes of data, the name first. */
	kern_memset(extended, 0, sizeof(extended));
	name_length = sizeof(name) - 1U;
	extended[0] = LOOPBACK_EVENT_EXTENDED_INQUIRY;
	extended[1] = 255U;
	extended[2] = 1U;
	extended[3] = 0x01U;
	extended[4] = 0x0eU;
	extended[5] = 0x0dU;
	extended[6] = 0x0cU;
	extended[7] = 0x0bU;
	extended[8] = 0x0aU;
	extended[11] = 0x40U;
	extended[12] = 0x25U;
	extended[13] = 0x00U;
	extended[16] = (uint8_t)(int8_t)-40;
	extended[17] = (uint8_t)(1U + name_length);
	extended[18] = 0x09U;
	kern_memcpy(extended + 19, name, name_length);
	error = loopback_queue(BT_PACKET_EVENT, extended, sizeof(extended));
	if (error != 0)
		return error;

	/* The result with RSSI: one response of 14 bytes. */
	kern_memset(plain, 0, sizeof(plain));
	plain[0] = LOOPBACK_EVENT_INQUIRY_RSSI;
	plain[1] = 15U;
	plain[2] = 1U;
	plain[3] = 0x02U;
	plain[4] = 0x0eU;
	plain[5] = 0x0dU;
	plain[6] = 0x0cU;
	plain[7] = 0x0bU;
	plain[8] = 0x0aU;
	plain[11] = 0x80U;
	plain[12] = 0x25U;
	plain[13] = 0x00U;
	plain[16] = (uint8_t)(int8_t)-60;
	error = loopback_queue(BT_PACKET_EVENT, plain, sizeof(plain));
	if (error != 0)
		return error;

	/* The inquiry's end. */
	complete[0] = LOOPBACK_EVENT_INQUIRY_COMPLETE;
	complete[1] = 1U;
	complete[2] = 0x00U;
	error = loopback_queue(BT_PACKET_EVENT, complete, sizeof(complete));
	if (error != 0)
		return error;

	/* Succeeded: the inquiry's packets wait. */
	return 0;
}

/*
 * Queues LE Set Scan Enable's answer and two advertising reports: a
 * public address 0A:0B:0C:0D:0E:03 with the flags, the complete name
 * "Loopback Mouse" and appearance 0x03C2 (RSSI -50), and a random address
 * 4A:0B:0C:0D:0E:04 with the flags alone (RSSI -70).
 */
static int
loopback_advertise(
	uint16_t opcode)
{
	static const uint8_t status_ok[1] = { 0x00U };
	static const char name[] = "Loopback Mouse";
	uint8_t first[2U + 3U + 9U + 31U + 1U];
	uint8_t second[2U + 3U + 9U + 3U + 1U];
	size_t name_length;
	size_t data;
	size_t at;
	int error;

	/* The answer. */
	error = loopback_complete(opcode, status_ok, sizeof(status_ok));
	if (error != 0)
		return error;

	/* The first report's data: the flags, the name, the appearance. */
	name_length = sizeof(name) - 1U;
	data = 3U + (2U + name_length) + 4U;
	at = 0U;
	first[at++] = LOOPBACK_EVENT_LE_META;
	first[at++] = (uint8_t)(2U + 9U + data + 1U);
	first[at++] = 0x02U;
	first[at++] = 1U;
	first[at++] = 0x00U;
	first[at++] = 0x00U;
	first[at++] = 0x03U;
	first[at++] = 0x0eU;
	first[at++] = 0x0dU;
	first[at++] = 0x0cU;
	first[at++] = 0x0bU;
	first[at++] = 0x0aU;
	first[at++] = (uint8_t)data;
	first[at++] = 2U;
	first[at++] = 0x01U;
	first[at++] = 0x06U;
	first[at++] = (uint8_t)(1U + name_length);
	first[at++] = 0x09U;
	kern_memcpy(first + at, name, name_length);
	at += name_length;
	first[at++] = 3U;
	first[at++] = 0x19U;
	first[at++] = 0xc2U;
	first[at++] = 0x03U;
	first[at++] = (uint8_t)(int8_t)-50;
	error = loopback_queue(BT_PACKET_EVENT, first, at);
	if (error != 0)
		return error;

	/* The second report: a random address, the flags alone. */
	at = 0U;
	second[at++] = LOOPBACK_EVENT_LE_META;
	second[at++] = (uint8_t)(2U + 9U + 3U + 1U);
	second[at++] = 0x02U;
	second[at++] = 1U;
	second[at++] = 0x00U;
	second[at++] = 0x01U;
	second[at++] = 0x04U;
	second[at++] = 0x0eU;
	second[at++] = 0x0dU;
	second[at++] = 0x0cU;
	second[at++] = 0x0bU;
	second[at++] = 0x4aU;
	second[at++] = 3U;
	second[at++] = 2U;
	second[at++] = 0x01U;
	second[at++] = 0x06U;
	second[at++] = (uint8_t)(int8_t)-70;
	error = loopback_queue(BT_PACKET_EVENT, second, at);
	if (error != 0)
		return error;

	/* Succeeded: the reports wait. */
	return 0;
}

/* Sleeps the calling thread for about the milliseconds given. */
static void
loopback_sleep_ms(
	unsigned milliseconds)
{
	uint64_t ticks;

	/* At least one tick. */
	ticks = kern_ms_to_ticks(milliseconds);
	if (ticks == 0U)
		ticks = 1U;

	/* Until the tick count passes the deadline. */
	sched_sleep(sched_ticks() + ticks);
}
