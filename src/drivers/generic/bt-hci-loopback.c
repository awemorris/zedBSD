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
 *   an ACL packet      the same packet back (handle 0x001)
 *
 * and plays devices to pair with (ws143-p004, plan/ws143/phase004/
 * phase.md section 8), the controller's side of Secure Simple Pairing
 * included.  The event masks and Simple Pairing's mode are kept: an event
 * the masks leave out is not sent (they start as the Core's defaults, and
 * Reset puts them back), and without Simple Pairing a link key's absence
 * asks for a PIN.  The BR/EDR devices are 0A:0B:0C:0D:0E:xx with xx 01
 * (DisplayYesNo, Numeric Comparison 123456), 05 (gives the debug key), 06
 * (a key of 7 bytes) and 07 (NoInputNoOutput, Just Works) on handle
 * 0x040, which answers L2CAP's Information Request; any other address is a
 * page timeout.  The LE device 0A:0B:0C:0D:0E:03 connects on handle 0x041
 * and refuses pairing (Pairing Not Supported); any other LE address never
 * connects (a cancel ends it).  Each ACL packet to a device's handle is
 * completed (Number Of Completed Packets).
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
#define LOOPBACK_OP_CONNECT		0x0405U
#define LOOPBACK_OP_DISCONNECT		0x0406U
#define LOOPBACK_OP_KEY_REPLY		0x040bU
#define LOOPBACK_OP_KEY_NEGATIVE	0x040cU
#define LOOPBACK_OP_PIN_NEGATIVE	0x040eU
#define LOOPBACK_OP_AUTHENTICATE	0x0411U
#define LOOPBACK_OP_ENCRYPT		0x0413U
#define LOOPBACK_OP_IO_REPLY		0x042bU
#define LOOPBACK_OP_CONFIRM_REPLY	0x042cU
#define LOOPBACK_OP_CONFIRM_NEGATIVE	0x042dU
#define LOOPBACK_OP_SET_EVENT_MASK	0x0c01U
#define LOOPBACK_OP_RESET		0x0c03U
#define LOOPBACK_OP_SSP_MODE		0x0c56U
#define LOOPBACK_OP_KEY_SIZE		0x1408U
#define LOOPBACK_OP_LE_EVENT_MASK	0x2001U
#define LOOPBACK_OP_LE_BUFFER_SIZE	0x2002U
#define LOOPBACK_OP_LE_CONNECT		0x200dU
#define LOOPBACK_OP_LE_CANCEL		0x200eU
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

/* The pairing's events. */
#define LOOPBACK_EVENT_CONNECTED	0x03U
#define LOOPBACK_EVENT_DISCONNECTED	0x05U
#define LOOPBACK_EVENT_AUTHENTICATED	0x06U
#define LOOPBACK_EVENT_ENCRYPTION	0x08U
#define LOOPBACK_EVENT_COMPLETED	0x13U
#define LOOPBACK_EVENT_PIN		0x16U
#define LOOPBACK_EVENT_KEY_REQUEST	0x17U
#define LOOPBACK_EVENT_KEY		0x18U
#define LOOPBACK_EVENT_IO_REQUEST	0x31U
#define LOOPBACK_EVENT_IO_RESPONSE	0x32U
#define LOOPBACK_EVENT_CONFIRM		0x33U
#define LOOPBACK_EVENT_SIMPLE_DONE	0x36U

/* The events the masks may leave out: codes 0x01 to 0x3E (bit code - 1); LE's subevents by the LE mask (bit subevent - 1). */
#define LOOPBACK_MASKABLE_LAST		0x3eU

/* The Core's default masks (Vol 4 Part E §7.3.1 and §7.8.1). */
#define LOOPBACK_MASK_DEFAULT		0x00001fffffffffffULL
#define LOOPBACK_LE_MASK_DEFAULT	0x000000000000001fULL

/* The devices' handles, and the last byte of each BR/EDR device's address 0A:0B:0C:0D:0E:xx. */
#define LOOPBACK_HANDLE_BREDR		0x0040U
#define LOOPBACK_HANDLE_LE		0x0041U
#define LOOPBACK_DEVICE_NUMERIC		0x01U
#define LOOPBACK_DEVICE_MOUSE		0x03U
#define LOOPBACK_DEVICE_DEBUG		0x05U
#define LOOPBACK_DEVICE_SHORT		0x06U
#define LOOPBACK_DEVICE_JUST_WORKS	0x07U

/* The IO capabilities, and the link key types (debug, unauthenticated and authenticated P-256). */
#define LOOPBACK_IO_DISPLAY_YES_NO	0x01U
#define LOOPBACK_IO_NONE		0x03U
#define LOOPBACK_KEY_DEBUG		0x03U
#define LOOPBACK_KEY_P256		0x07U
#define LOOPBACK_KEY_P256_MITM		0x08U

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
 * worker's requests (work, withdraw), the masks and Simple Pairing's mode
 * (event_mask, le_mask, simple_pairing) and hci.  The device being paired
 * (device, the host's IO capability, the key type given) is written and
 * read by the command path alone (writes to the node are one at a time).  deliver is held by the
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
	uint64_t event_mask;
	uint64_t le_mask;
	unsigned simple_pairing;
	uint8_t device[6];
	uint8_t host_io;
	uint8_t key_type;
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
static void loopback_defaults(void);
static int loopback_masked(const uint8_t *body, size_t length);
static int loopback_pairing(uint16_t opcode, const uint8_t *parameters, size_t length, int *handled);
static int loopback_event(uint8_t code, const uint8_t *parameters, size_t length);
static int loopback_address_event(uint8_t code, const uint8_t *more, size_t more_length);
static int loopback_complete_address(uint16_t opcode);
static int loopback_peer_acl(const uint8_t *packet, size_t length);
static int loopback_frame(uint16_t handle, uint16_t cid, const uint8_t *payload, size_t length);
static uint8_t loopback_role(void);

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
	loopback_defaults();

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
	int masked;
	int error;

	/* At the end of the waiting packets, unless the masks leave the event out. */
	irq = spin_lock_irqsave(&loopback.lock);

	error = 0;
	masked = 0;
	if (type == BT_PACKET_EVENT)
		masked = loopback_masked(body, length);
	if (!masked)
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
	static const uint8_t features[9] = { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x40U, 0x00U, 0x40U, 0x00U };
	static const uint8_t buffer_size[8] = { 0x00U, 0xfdU, 0x03U, 0x00U, 0x08U, 0x00U, 0x00U, 0x00U };
	uint8_t commands[1U + 64U];
	unsigned long irq;
	uint16_t opcode;
	unsigned handle;
	unsigned count;
	unsigned delay;
	int handled;
	int error;

	UNUSED_PARAMETER(context);

	/* An ACL packet goes to a device, or comes back as it went; a command has its opcode. */
	error = 0;
	opcode = 0U;
	if (packet[0] == BT_PACKET_ACL) {
		handle = (unsigned)packet[1] | ((unsigned)(packet[2] & 0x0fU) << 8);
		if (handle == LOOPBACK_HANDLE_BREDR || handle == LOOPBACK_HANDLE_LE) {
			error = loopback_peer_acl(packet, length);
		} else {
			error = loopback_queue(BT_PACKET_ACL, packet + 1, length - 1U);
		}
	} else {
		opcode = (uint16_t)(packet[1] | (packet[2] << 8));
	}

	/* The pairing's commands, and the masks and modes. */
	handled = 0;
	if (opcode != 0U)
		error = loopback_pairing(opcode, packet + 4, length - 4U, &handled);
	if (handled)
		opcode = 0U;

	/* Each command the tests use (none for an ACL packet). */
	switch (opcode) {
	case 0U:
		/* The ACL packet's echo, or the pairing's answers, are queued already. */
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
		/* LE Supported (Controller): byte 4, bit 6; Non-flushable Packet Boundary Flag: byte 6, bit 6. */
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

	/* The masks and Simple Pairing's mode as at power-on. */
	loopback_defaults();

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

/* Puts the event masks and Simple Pairing's mode as at power-on (the Core's defaults, Simple Pairing off). */
static void
loopback_defaults(
	void)
{
	unsigned long irq;

	/* The defaults. */
	irq = spin_lock_irqsave(&loopback.lock);

	loopback.event_mask = LOOPBACK_MASK_DEFAULT;
	loopback.le_mask = LOOPBACK_LE_MASK_DEFAULT;
	loopback.simple_pairing = 0U;

	spin_unlock_irqrestore(&loopback.lock, irq);
}

/*
 * Tells whether the masks leave an event out (the lock is held): an event
 * of code 1 to 0x3E whose bit (code - 1) is clear, or an LE subevent whose
 * LE bit (subevent - 1) is clear.  Command Complete and Status, Number Of
 * Completed Packets and the vendor events are never left out.
 */
static int
loopback_masked(
	const uint8_t *body,
	size_t length)
{
	unsigned code;
	unsigned subevent;

	/* The events the masks do not cover. */
	code = body[0];
	if (code == 0U || code > LOOPBACK_MASKABLE_LAST)
		return 0;
	if (code == LOOPBACK_EVENT_COMPLETE || code == LOOPBACK_EVENT_STATUS || code == LOOPBACK_EVENT_COMPLETED)
		return 0;

	/* The event's bit. */
	if ((loopback.event_mask & (1ULL << (code - 1U))) == 0U)
		return 1;

	/* An LE subevent's bit, too. */
	if (code == LOOPBACK_EVENT_LE_META && length >= 3U) {
		subevent = body[2];
		if (subevent >= 1U && subevent <= 64U && (loopback.le_mask & (1ULL << (subevent - 1U))) == 0U)
			return 1;
	}

	/* Sent. */
	return 0;
}

/*
 * Answers the commands of the masks, the modes and the pairing as the
 * devices it plays (see the file's comment).  Sets *handled for a command
 * it answered.  Returns 0 or the queue's error.
 */
static int
loopback_pairing(
	uint16_t opcode,
	const uint8_t *parameters,
	size_t length,
	int *handled)
{
	static const uint8_t status_ok[1] = { 0x00U };
	static const uint8_t le_buffers[4] = { 0x00U, 27U, 0x00U, 4U };
	uint8_t event[24];
	uint8_t key[17];
	unsigned long irq;
	uint64_t mask;
	unsigned index;
	uint8_t role;
	int differs;
	int error;

	/* Each command of the pairing; any other is the caller's. */
	*handled = 1;
	role = loopback_role();
	error = 0;
	switch (opcode) {
	case LOOPBACK_OP_SET_EVENT_MASK:
	case LOOPBACK_OP_LE_EVENT_MASK:
		/* A mask of 8 bytes, least significant first, kept. */
		if (length < 8U)
			return EINVAL;
		mask = 0U;
		for (index = 0U; index < 8U; index++)
			mask |= (uint64_t)parameters[index] << (8U * index);
		irq = spin_lock_irqsave(&loopback.lock);

		if (opcode == LOOPBACK_OP_SET_EVENT_MASK)
			loopback.event_mask = mask;
		else
			loopback.le_mask = mask;

		spin_unlock_irqrestore(&loopback.lock, irq);

		/* Done. */
		error = loopback_complete(opcode, status_ok, sizeof(status_ok));
		break;
	case LOOPBACK_OP_SSP_MODE:
		/* Simple Pairing's mode, kept. */
		if (length < 1U)
			return EINVAL;
		irq = spin_lock_irqsave(&loopback.lock);

		loopback.simple_pairing = parameters[0];

		spin_unlock_irqrestore(&loopback.lock, irq);

		/* Done. */
		error = loopback_complete(opcode, status_ok, sizeof(status_ok));
		break;
	case LOOPBACK_OP_RESET:
		/* The masks and the mode as at power-on. */
		loopback_defaults();
		error = loopback_complete(opcode, status_ok, sizeof(status_ok));
		break;
	case LOOPBACK_OP_LE_BUFFER_SIZE:
		/* LE's own buffers: 27 bytes, 4 packets. */
		error = loopback_complete(opcode, le_buffers, sizeof(le_buffers));
		break;
	case LOOPBACK_OP_CONNECT:
		/* The device paged; a device the loopback does not play does not answer (Page Timeout). */
		if (length < 6U)
			return EINVAL;
		kern_memcpy(loopback.device, parameters, 6U);
		role = loopback_role();
		error = loopback_status(opcode);
		event[0] = 0x04U;
		if (role == LOOPBACK_DEVICE_NUMERIC ||
		    role == LOOPBACK_DEVICE_DEBUG ||
		    role == LOOPBACK_DEVICE_SHORT ||
		    role == LOOPBACK_DEVICE_JUST_WORKS)
			event[0] = 0x00U;
		event[1] = (uint8_t)(LOOPBACK_HANDLE_BREDR & 0xffU);
		event[2] = (uint8_t)(LOOPBACK_HANDLE_BREDR >> 8);
		kern_memcpy(event + 3, loopback.device, 6U);
		event[9] = 0x01U;
		event[10] = 0x00U;
		if (error == 0)
			error = loopback_event(LOOPBACK_EVENT_CONNECTED, event, 11U);
		break;
	case LOOPBACK_OP_AUTHENTICATE:
		/* The controller asks the host for a link key. */
		error = loopback_status(opcode);
		if (error == 0)
			error = loopback_address_event(LOOPBACK_EVENT_KEY_REQUEST, NULL, 0U);
		break;
	case LOOPBACK_OP_KEY_REPLY:
		/* The host's key: the device's own gives the authentication, another is a missing key. */
		if (length < 22U)
			return EINVAL;
		error = loopback_complete_address(opcode);
		kern_memset(key, (int)(0xa0U + role), 16U);
		differs = kern_memcmp(parameters + 6, key, 16U);
		event[0] = 0x00U;
		if (differs != 0)
			event[0] = 0x06U;
		event[1] = (uint8_t)(LOOPBACK_HANDLE_BREDR & 0xffU);
		event[2] = (uint8_t)(LOOPBACK_HANDLE_BREDR >> 8);
		if (error == 0)
			error = loopback_event(LOOPBACK_EVENT_AUTHENTICATED, event, 3U);
		break;
	case LOOPBACK_OP_KEY_NEGATIVE:
		/* No key: Simple Pairing asks for the IO capability, or else a PIN. */
		error = loopback_complete_address(opcode);
		if (error != 0)
			break;
		irq = spin_lock_irqsave(&loopback.lock);

		index = loopback.simple_pairing;

		spin_unlock_irqrestore(&loopback.lock, irq);

		/* The next question. */
		if (index != 0U)
			error = loopback_address_event(LOOPBACK_EVENT_IO_REQUEST, NULL, 0U);
		else
			error = loopback_address_event(LOOPBACK_EVENT_PIN, NULL, 0U);
		break;
	case LOOPBACK_OP_PIN_NEGATIVE:
		/* No PIN: the authentication fails (PIN or Key Missing). */
		error = loopback_complete_address(opcode);
		event[0] = 0x06U;
		event[1] = (uint8_t)(LOOPBACK_HANDLE_BREDR & 0xffU);
		event[2] = (uint8_t)(LOOPBACK_HANDLE_BREDR >> 8);
		if (error == 0)
			error = loopback_event(LOOPBACK_EVENT_AUTHENTICATED, event, 3U);
		break;
	case LOOPBACK_OP_IO_REPLY:
		/* The host's IO capability; the device's, then the number to compare (0 for Just Works). */
		if (length < 9U)
			return EINVAL;
		loopback.host_io = parameters[6];
		error = loopback_complete_address(opcode);
		event[0] = LOOPBACK_IO_DISPLAY_YES_NO;
		if (role == LOOPBACK_DEVICE_JUST_WORKS)
			event[0] = LOOPBACK_IO_NONE;
		event[1] = 0x00U;
		event[2] = 0x03U;
		if (error == 0)
			error = loopback_address_event(LOOPBACK_EVENT_IO_RESPONSE, event, 3U);

		/* The number: 123456, or 0 for Just Works. */
		event[0] = 0x00U;
		event[1] = 0x00U;
		event[2] = 0x00U;
		event[3] = 0x00U;
		if (role != LOOPBACK_DEVICE_JUST_WORKS) {
			event[0] = 0x40U;
			event[1] = 0xe2U;
			event[2] = 0x01U;
		}

		/* The question to the host. */
		if (error == 0)
			error = loopback_address_event(LOOPBACK_EVENT_CONFIRM, event, 4U);
		break;
	case LOOPBACK_OP_CONFIRM_REPLY:
		/* Paired: the link key (its type from the association) and the authentication's end. */
		error = loopback_complete_address(opcode);
		loopback.key_type = LOOPBACK_KEY_P256_MITM;
		if (role == LOOPBACK_DEVICE_JUST_WORKS || loopback.host_io == LOOPBACK_IO_NONE)
			loopback.key_type = LOOPBACK_KEY_P256;
		if (role == LOOPBACK_DEVICE_DEBUG)
			loopback.key_type = LOOPBACK_KEY_DEBUG;
		event[0] = 0x00U;
		kern_memcpy(event + 1, loopback.device, 6U);
		if (error == 0)
			error = loopback_event(LOOPBACK_EVENT_SIMPLE_DONE, event, 7U);
		kern_memset(key, (int)(0xa0U + role), 16U);
		key[16] = loopback.key_type;
		if (error == 0)
			error = loopback_address_event(LOOPBACK_EVENT_KEY, key, 17U);
		event[0] = 0x00U;
		event[1] = (uint8_t)(LOOPBACK_HANDLE_BREDR & 0xffU);
		event[2] = (uint8_t)(LOOPBACK_HANDLE_BREDR >> 8);
		if (error == 0)
			error = loopback_event(LOOPBACK_EVENT_AUTHENTICATED, event, 3U);
		break;
	case LOOPBACK_OP_CONFIRM_NEGATIVE:
		/* Refused: Simple Pairing and the authentication fail (Authentication Failure). */
		error = loopback_complete_address(opcode);
		event[0] = 0x05U;
		kern_memcpy(event + 1, loopback.device, 6U);
		if (error == 0)
			error = loopback_event(LOOPBACK_EVENT_SIMPLE_DONE, event, 7U);
		event[0] = 0x05U;
		event[1] = (uint8_t)(LOOPBACK_HANDLE_BREDR & 0xffU);
		event[2] = (uint8_t)(LOOPBACK_HANDLE_BREDR >> 8);
		if (error == 0)
			error = loopback_event(LOOPBACK_EVENT_AUTHENTICATED, event, 3U);
		break;
	case LOOPBACK_OP_ENCRYPT:
		/* On: AES-CCM (0x02) on a Secure Connections key, E0 (0x01) otherwise. */
		error = loopback_status(opcode);
		event[0] = 0x00U;
		event[1] = (uint8_t)(LOOPBACK_HANDLE_BREDR & 0xffU);
		event[2] = (uint8_t)(LOOPBACK_HANDLE_BREDR >> 8);
		event[3] = 0x01U;
		if (loopback.key_type == LOOPBACK_KEY_P256 || loopback.key_type == LOOPBACK_KEY_P256_MITM)
			event[3] = 0x02U;
		if (error == 0)
			error = loopback_event(LOOPBACK_EVENT_ENCRYPTION, event, 4U);
		break;
	case LOOPBACK_OP_KEY_SIZE:
		/* Status, handle, size: 16, or 7 for the short device. */
		if (length < 2U)
			return EINVAL;
		event[0] = 0x00U;
		event[1] = parameters[0];
		event[2] = parameters[1];
		event[3] = 16U;
		if (role == LOOPBACK_DEVICE_SHORT)
			event[3] = 7U;
		error = loopback_complete(opcode, event, 4U);
		break;
	case LOOPBACK_OP_DISCONNECT:
		/* Ended by the local host. */
		if (length < 2U)
			return EINVAL;
		error = loopback_status(opcode);
		event[0] = 0x00U;
		event[1] = parameters[0];
		event[2] = parameters[1];
		event[3] = 0x16U;
		if (error == 0)
			error = loopback_event(LOOPBACK_EVENT_DISCONNECTED, event, 4U);
		break;
	case LOOPBACK_OP_LE_CONNECT:
		/* The LE mouse (public) connects; any other address never does. */
		if (length < 12U)
			return EINVAL;
		error = loopback_status(opcode);
		if (error != 0 ||
		    parameters[5] != 0x00U ||
		    parameters[6] != LOOPBACK_DEVICE_MOUSE ||
		    parameters[7] != 0x0eU ||
		    parameters[8] != 0x0dU ||
		    parameters[9] != 0x0cU ||
		    parameters[10] != 0x0bU ||
		    parameters[11] != 0x0aU)
			break;
		kern_memset(event, 0, sizeof(event));
		event[0] = 0x01U;
		event[1] = 0x00U;
		event[2] = (uint8_t)(LOOPBACK_HANDLE_LE & 0xffU);
		event[3] = (uint8_t)(LOOPBACK_HANDLE_LE >> 8);
		event[4] = 0x00U;
		event[5] = 0x00U;
		kern_memcpy(event + 6, parameters + 6, 6U);
		event[12] = 0x18U;
		event[16] = 0xf4U;
		event[17] = 0x01U;
		error = loopback_event(LOOPBACK_EVENT_LE_META, event, 19U);
		break;
	case LOOPBACK_OP_LE_CANCEL:
		/* The connection under way ends: LE Connection Complete, Unknown Connection Identifier. */
		error = loopback_complete(opcode, status_ok, sizeof(status_ok));
		kern_memset(event, 0, sizeof(event));
		event[0] = 0x01U;
		event[1] = 0x02U;
		if (error == 0)
			error = loopback_event(LOOPBACK_EVENT_LE_META, event, 19U);
		break;
	default:
		/* Not the pairing's. */
		*handled = 0;
		break;
	}

	/* Reports a full queue. */
	if (error != 0)
		return error;

	/* Succeeded: the answers wait for the worker. */
	return 0;
}

/* Queues an event of a code with its parameters. */
static int
loopback_event(
	uint8_t code,
	const uint8_t *parameters,
	size_t length)
{
	uint8_t event[2U + 64U];
	int error;

	/* The code, the length, the parameters. */
	if (length > 64U)
		return EINVAL;
	event[0] = code;
	event[1] = (uint8_t)length;
	kern_memcpy(event + 2, parameters, length);

	/* Queued (unless the masks leave it out). */
	error = loopback_queue(BT_PACKET_EVENT, event, 2U + length);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Queues an event led by the device's address, with more parameters. */
static int
loopback_address_event(
	uint8_t code,
	const uint8_t *more,
	size_t more_length)
{
	uint8_t parameters[6U + 24U];
	int error;

	/* The address, then the rest. */
	if (more_length > 24U)
		return EINVAL;
	kern_memcpy(parameters, loopback.device, 6U);
	if (more_length != 0U)
		kern_memcpy(parameters + 6, more, more_length);

	/* Queued. */
	error = loopback_event(code, parameters, 6U + more_length);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Queues a Command Complete whose return parameters are the status and the device's address. */
static int
loopback_complete_address(
	uint16_t opcode)
{
	uint8_t returned[7];
	int error;

	/* Status 0, the address. */
	returned[0] = 0x00U;
	kern_memcpy(returned + 1, loopback.device, 6U);
	error = loopback_complete(opcode, returned, sizeof(returned));
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Takes an ACL packet the host sent to a device: completed at once, and
 * its L2CAP frame (one packet in this test's frames) answered: BR/EDR's
 * Information Request with the features, LE's Pairing Request with Pairing
 * Failed (Pairing Not Supported).
 */
static int
loopback_peer_acl(
	const uint8_t *packet,
	size_t length)
{
	uint8_t completed[5];
	uint8_t answer[12];
	const uint8_t *payload;
	unsigned handle;
	unsigned cid;
	int error;

	/* The packet's handle, completed. */
	handle = (unsigned)packet[1] | ((unsigned)(packet[2] & 0x0fU) << 8);
	completed[0] = 1U;
	completed[1] = (uint8_t)(handle & 0xffU);
	completed[2] = (uint8_t)(handle >> 8);
	completed[3] = 1U;
	completed[4] = 0U;
	error = loopback_event(LOOPBACK_EVENT_COMPLETED, completed, sizeof(completed));
	if (error != 0)
		return error;

	/* An L2CAP header and a command's first bytes, or nothing to answer. */
	if (length < 1U + 4U + 4U + 2U)
		return 0;
	cid = (unsigned)packet[7] | ((unsigned)packet[8] << 8);
	payload = packet + 9;

	/* BR/EDR's Information Request: the extended features (fixed channels). */
	if (handle == LOOPBACK_HANDLE_BREDR && cid == 0x0001U && payload[0] == 0x0aU && length >= 1U + 4U + 4U + 6U) {
		kern_memset(answer, 0, sizeof(answer));
		answer[0] = 0x0bU;
		answer[1] = payload[1];
		answer[2] = 8U;
		answer[4] = payload[4];
		answer[5] = payload[5];
		answer[8] = 0x80U;
		error = loopback_frame(LOOPBACK_HANDLE_BREDR, 0x0001U, answer, sizeof(answer));
		return error;
	}

	/* LE's Pairing Request: refused. */
	if (handle == LOOPBACK_HANDLE_LE && cid == 0x0006U && payload[0] == 0x01U) {
		answer[0] = 0x05U;
		answer[1] = 0x05U;
		error = loopback_frame(LOOPBACK_HANDLE_LE, 0x0006U, answer, 2U);
		return error;
	}

	/* Nothing to answer. */
	return 0;
}

/* Queues an L2CAP frame to the host in one ACL packet (a first, flushable packet). */
static int
loopback_frame(
	uint16_t handle,
	uint16_t cid,
	const uint8_t *payload,
	size_t length)
{
	uint8_t body[4U + 4U + 16U];
	int error;

	/* The ACL header, the L2CAP header, the payload. */
	if (length > 16U)
		return EINVAL;
	body[0] = (uint8_t)(handle & 0xffU);
	body[1] = (uint8_t)(((handle >> 8) & 0x0fU) | 0x20U);
	body[2] = (uint8_t)((4U + length) & 0xffU);
	body[3] = 0U;
	body[4] = (uint8_t)(length & 0xffU);
	body[5] = 0U;
	body[6] = (uint8_t)(cid & 0xffU);
	body[7] = (uint8_t)(cid >> 8);
	kern_memcpy(body + 8, payload, length);

	/* Queued. */
	error = loopback_queue(BT_PACKET_ACL, body, 8U + length);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Gives the last byte of the device's address when it is 0A:0B:0C:0D:0E:xx, else 0 (a device the loopback does not play). */
static uint8_t
loopback_role(
	void)
{
	/* The address's fixed part. */
	if (loopback.device[5] != 0x0aU ||
	    loopback.device[4] != 0x0bU ||
	    loopback.device[3] != 0x0cU ||
	    loopback.device[2] != 0x0dU ||
	    loopback.device[1] != 0x0eU)
		return 0U;

	/* The device's byte. */
	return loopback.device[0];
}
