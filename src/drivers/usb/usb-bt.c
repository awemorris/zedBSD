/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The USB transport of the Bluetooth controllers (ws143-p002; the node's
 * interface is <uapi/bluetooth.h>, the class drivers/generic/bt-hci.h).
 *
 * It takes an interface of class E0/01/01 that has an interrupt IN, a
 * bulk IN and a bulk OUT endpoint (the controller's second interface, of
 * the same class, has only isochronous endpoints and is left alone).  A
 * command goes on the default control pipe as a class request, an ACL
 * packet on bulk OUT; events come on interrupt IN and ACL packets on bulk
 * IN.  While the Intel bootloader's path is on, the Secure Send command
 * goes on bulk OUT and bulk IN carries events.
 *
 * Each IN pipe has one transfer.  Its completion only marks the pipe done
 * and wakes the worker, which drains the transfer, cuts the bytes into
 * packets (a packet may span transfers) and hands them to the class.  A
 * packet the class has no room for stays with the pipe, and the pipe's
 * transfer is not submitted again until the packet and the rest of the
 * bytes are taken: the controller then holds what it has (the endpoint
 * NAKs), and nothing is dropped.  A read makes room and the class's room
 * call wakes the worker to try again.  A transfer that stalls or fails on
 * the bus leaves its endpoint halted: the halt is cleared (on the device
 * and in the host controller) and the transfer submitted again, until the
 * same pipe fails USB_BT_ERRORS_MAX times in a row.
 *
 * The reset (BT_IOC_RESET) pauses the worker, cancels and drains the
 * transfers, resets the USB device in place, and starts the transfers
 * again.  The detach withdraws the class first (no operation runs after
 * it), stops the transfers and the worker, then releases the class.
 *
 * SCO (ws197-p007a2, plan/ws197/phase007/phase.md A1 to A7): the attach
 * claims the controller's isochronous interface (left at alternate 0).
 * BT_IOC_SET_SCO selects the alternate its links need and starts the SCO
 * thread (priority 4), which keeps three IN transfers of 8 packets and up
 * to four OUT transfers of 4 packets on the endpoints.  Their completions
 * only wake it.  It puts the IN bytes together into SCO packets for the
 * class (a missed packet drops the one being gathered), and fills the OUT
 * transfers from the queue the program's writes fill (sco_send, which
 * never sleeps).  Before the alternate changes, and at the reset, the
 * detach and the close, the thread stops and every SCO transfer is
 * cancelled and drained.
 */

#include <drivers/generic/bt-hci.h>
#include <drivers/usb/usb-bt.h>
#include <drivers/usb/usb.h>

#include <kern/clock.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <uapi/errno.h>

/* Marks a parameter a function takes for its signature's sake. */
#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(parameter) ((void)(parameter))
#endif

/* How long a command and an ACL packet may take to be sent. */
#define USB_BT_COMMAND_MS		1000U
#define USB_BT_ACL_MS			2000U

/* How long the worker or the reset waits for a cancelled transfer to retire. */
#define USB_BT_DRAIN_MS			1000U

/* How long the reset waits for the worker to set its transfers aside, and how often it looks. */
#define USB_BT_QUIESCE_MS		2000U
#define USB_BT_QUIESCE_STEP_MS		5U

/* How many times the reset tries a device the USB core says is busy, and the wait between. */
#define USB_BT_RESET_TRIES		5U
#define USB_BT_RESET_RETRY_MS		50U

/* The bulk IN transfer's size (a multiple of every bulk endpoint's packet size). */
#define USB_BT_BULK_BUFFER		4096U

/* The packet size's bits of wMaxPacketSize (bits 11 and 12 count a high-speed endpoint's extra transactions). */
#define USB_BT_PACKET_SIZE_MASK		0x07ffU

/* Failed transfers in a row before a pipe is given up. */
#define USB_BT_ERRORS_MAX		8U

/* The worker's requests, set under the lock and taken by the worker. */
#define USB_BT_WORK_EVENT_DONE		0x0001U
#define USB_BT_WORK_DATA_DONE		0x0002U
#define USB_BT_WORK_ROOM		0x0004U
#define USB_BT_WORK_MODE		0x0008U
#define USB_BT_WORK_QUIESCE		0x0010U
#define USB_BT_WORK_ARM			0x0020U

/* SCO: the SCO thread's priority, its transfers and their packets, and the send queue in SCO packets. */
#define USB_BT_SCO_PRIORITY		4
#define USB_BT_SCO_IN_URBS		3U
#define USB_BT_SCO_IN_PACKETS		8U
#define USB_BT_SCO_OUT_URBS		4U
#define USB_BT_SCO_OUT_PACKETS		4U
#define USB_BT_SCO_QUEUE_PACKETS	12U
#define USB_BT_SCO_QUEUE_BYTES		(USB_BT_SCO_QUEUE_PACKETS * (BT_SCO_HEADER + BT_SCO_DATA_MAX))

/* The isochronous interface's alternate for a transparent link, and how long a busy SET_INTERFACE is tried. */
#define USB_BT_SCO_TRANSPARENT_ALTERNATE	6U
#define USB_BT_SCO_ALTERNATE_TRIES	10U
#define USB_BT_SCO_ALTERNATE_STEP_MS	5U

/* The SCO thread's requests: a transfer done (IN or OUT, by bit), bytes to send, the stop. */
#define USB_BT_SCO_WORK_SEND		0x80000000U

/* The two IN pipes. */
#define USB_BT_PIPE_EVENT		0U
#define USB_BT_PIPE_DATA		1U
#define USB_BT_PIPES			2U

/*
 * One IN pipe and its transfer.
 *
 * armed (under the controller's lock) is set while the transfer is
 * submitted and not yet finished by the worker; dead once the pipe is
 * given up.  The rest belongs to the worker: the bytes of the last
 * transfer not yet taken (held_offset up to held_length), whether a stall
 * was counted for the current refusal, and the failures in a row.
 */
struct usb_bt_pipe {
	struct drv_usb_endpoint *endpoint;
	struct drv_usb_urb *urb;
	uint8_t *buffer;
	size_t buffer_size;
	uint8_t *assembly;
	struct bt_hci_assembler assembler;
	size_t held_offset;
	size_t held_length;
	unsigned armed;
	unsigned dead;
	unsigned stalled;
	unsigned errors;
	unsigned done_work;
	const char *name;
};

/*
 * One controller.
 *
 * lock guards work, stopping, paused, quiesced, bootloader, submitting and
 * the pipes' armed and dead.  stopping tells the worker to leave (the
 * detach) and admits no more submissions; submitting counts the
 * submissions admitted before it and still under way, which the detach
 * waits out before it cancels the transfers;
 * paused keeps the transfers from being submitted while the reset runs,
 * and quiesced is the worker's answer that none is submitted and nothing
 * is held.  bootloader is the path the class last set; the worker reads
 * the bulk IN pipe as events while it is set.
 */
struct usb_bt {
	struct drv_usb_interface *interface;
	struct drv_usb_device *device;
	struct drv_usb_endpoint *bulk_out;
	struct usb_bt_pipe pipes[USB_BT_PIPES];
	struct drv_bt_hci *hci;
	struct spinlock lock;
	unsigned work;
	unsigned stopping;
	unsigned paused;
	unsigned quiesced;
	unsigned bootloader;
	unsigned submitting;
	struct thread *worker;
	uint8_t *out;
	char name[BT_TEXT_MAX];
	char physical_path[BT_TEXT_MAX];

	/*
	 * SCO (ws197-p007a2).  sco_interface is the claimed isochronous
	 * interface (NULL without SCO), sco_alternate the alternate now and
	 * sco_mtu its packets' size.  Under lock: sco_streaming (sco_send
	 * takes packets), sco_stopping, sco_work (a bit a transfer: IN 0 to 2,
	 * OUT 8 to 11, and USB_BT_SCO_WORK_SEND), the transfers submitted
	 * (sco_in_busy, sco_out_busy) and the send queue.  The rest is the SCO
	 * thread's, or sco_set's while the thread is stopped.
	 */
	struct drv_usb_interface *sco_interface;
	unsigned sco_transparent;
	unsigned sco_alternate;
	size_t sco_mtu;
	unsigned sco_streaming;
	unsigned sco_stopping;
	unsigned sco_work;
	unsigned sco_in_busy;
	unsigned sco_out_busy;
	struct thread *sco_thread;
	struct drv_usb_endpoint *sco_in;
	struct drv_usb_endpoint *sco_out;
	struct drv_usb_urb *sco_in_urbs[USB_BT_SCO_IN_URBS];
	struct drv_usb_urb *sco_out_urbs[USB_BT_SCO_OUT_URBS];
	uint8_t *sco_in_buffers[USB_BT_SCO_IN_URBS];
	uint8_t *sco_out_buffers[USB_BT_SCO_OUT_URBS];
	struct bt_hci_sco_assembler sco_assembler;
	struct bt_hci_fifo sco_queue;
	size_t sco_queue_limit;
	uint8_t sco_queue_bytes[USB_BT_SCO_QUEUE_BYTES];
	uint64_t sco_dropped;
};

static int usb_bt_match(struct drv_usb_interface *interface, const struct drv_usb_id *id);
static int usb_bt_attach(struct drv_usb_interface *interface, const struct drv_usb_id *id);
static void usb_bt_log_sco(const struct usb_bt *bt, struct drv_usb_interface *interface);
static int usb_bt_detach(struct drv_usb_interface *interface, unsigned flags);
static int usb_bt_pipe_prepare(struct usb_bt *bt, struct usb_bt_pipe *pipe, struct drv_usb_endpoint *endpoint, size_t buffer_size, unsigned mode);
static void usb_bt_pipe_free(struct usb_bt_pipe *pipe);
static void usb_bt_free(struct usb_bt *bt);
static void usb_bt_identity(struct usb_bt *bt);
static int usb_bt_stop_transfers(struct usb_bt *bt);
static int usb_bt_join_worker(struct usb_bt *bt);
static void usb_bt_completion(struct drv_usb_urb *urb, void *argument);
static void usb_bt_request(struct usb_bt *bt, unsigned work);
static void usb_bt_worker(void *argument);
static void usb_bt_finish(struct usb_bt *bt, struct usb_bt_pipe *pipe);
static int usb_bt_flush(struct usb_bt *bt, struct usb_bt_pipe *pipe);
static void usb_bt_arm(struct usb_bt *bt, struct usb_bt_pipe *pipe);
static void usb_bt_give_up(struct usb_bt *bt, struct usb_bt_pipe *pipe, const char *stage, int error);
static void usb_bt_set_aside(struct usb_bt *bt);
static void usb_bt_change_mode(struct usb_bt *bt);
static int usb_bt_deliver(void *context, uint8_t type, const uint8_t *packet, size_t length);
static int usb_bt_send(void *context, const uint8_t *packet, size_t length);
static int usb_bt_set_bootloader(void *context, int on);
static int usb_bt_reset(void *context);
static void usb_bt_room(void *context);
static int usb_bt_quiesce(struct usb_bt *bt);
static void usb_bt_sleep_ms(unsigned milliseconds);
static void usb_bt_pipe_restart(struct usb_bt_pipe *pipe);
static void usb_bt_sco_claim(struct usb_bt *bt);
static int usb_bt_sco_set(void *context, const struct bt_sco *links, struct bt_sco *answer);
static int usb_bt_sco_send(void *context, const uint8_t *packet, size_t length);
static int usb_bt_sco_stop(struct usb_bt *bt);
static int usb_bt_sco_select(struct usb_bt *bt, unsigned alternate);
static int usb_bt_sco_start(struct usb_bt *bt, const struct bt_sco *links);
static void usb_bt_sco_free(struct usb_bt *bt);
static void usb_bt_sco_completion(struct drv_usb_urb *urb, void *argument);
static void usb_bt_sco_thread(void *argument);
static void usb_bt_sco_receive(struct usb_bt *bt, unsigned index);
static void usb_bt_sco_submit_in(struct usb_bt *bt, unsigned index);
static void usb_bt_sco_fill(struct usb_bt *bt);
static void usb_bt_sco_deliver(void *context, const uint8_t *packet, size_t length);
static int usb_bt_sco_retire(struct drv_usb_urb *urb);

/* The interfaces the driver takes: a Bluetooth controller's HCI, E0/01/01. */
static const struct drv_usb_id usb_bt_ids[] = {
	{
		.match_flags = DRV_USB_ID_IF_CLASS | DRV_USB_ID_IF_SUBCLASS | DRV_USB_ID_IF_PROTOCOL,
		.interface_class = USB_BT_INTERFACE_CLASS,
		.interface_subclass = USB_BT_INTERFACE_SUBCLASS,
		.interface_protocol = USB_BT_INTERFACE_PROTOCOL
	}
};

/* The driver, registered once at boot. */
static struct drv_usb_driver usb_bt_driver = {
	.name = "usb-bt",
	.ids = usb_bt_ids,
	.id_count = sizeof(usb_bt_ids) / sizeof(usb_bt_ids[0]),
	.match = usb_bt_match,
	.attach = usb_bt_attach,
	.detach = usb_bt_detach
};

/* What a controller does for the HCI class. */
static const struct drv_bt_hci_ops usb_bt_ops = {
	.send = usb_bt_send,
	.set_bootloader = usb_bt_set_bootloader,
	.reset = usb_bt_reset,
	.room = usb_bt_room,
	.sco_set = usb_bt_sco_set,
	.sco_send = usb_bt_sco_send
};

/*
 * Registers the driver with the USB core.
 */
int
drv_usb_bt_driver_register(
	void)
{
	int error;

	/* The core matches it against every interface from now on. */
	error = drv_usb_driver_register(&usb_bt_driver);
	if (error != 0)
		return error;

	/* Succeeded: the driver is registered. */
	return 0;
}

/* Takes an interface with the HCI's three endpoints. */
static int
usb_bt_match(
	struct drv_usb_interface *interface,
	const struct drv_usb_id *id)
{
	struct drv_usb_endpoint *event_in;
	struct drv_usb_endpoint *data_in;
	struct drv_usb_endpoint *data_out;

	UNUSED_PARAMETER(id);

	/* The events' interrupt IN endpoint (the isochronous interface has none). */
	event_in = drv_usb_interface_find_endpoint(interface, DRV_USB_TRANSFER_INTERRUPT, DRV_USB_DIR_IN, NULL);
	if (event_in == NULL)
		return 0;

	/* The ACL packets' bulk IN endpoint. */
	data_in = drv_usb_interface_find_endpoint(interface, DRV_USB_TRANSFER_BULK, DRV_USB_DIR_IN, NULL);
	if (data_in == NULL)
		return 0;

	/* The ACL packets' bulk OUT endpoint. */
	data_out = drv_usb_interface_find_endpoint(interface, DRV_USB_TRANSFER_BULK, DRV_USB_DIR_OUT, NULL);
	if (data_out == NULL)
		return 0;

	/* This driver takes it. */
	return 100;
}

/* Binds a controller: its pipes, its buffers, its node and its worker. */
static int
usb_bt_attach(
	struct drv_usb_interface *interface,
	const struct drv_usb_id *id)
{
	const struct drv_usb_device_descriptor *descriptor;
	struct drv_bt_hci_description description;
	struct drv_usb_endpoint *event_in;
	struct drv_usb_endpoint *data_in;
	struct usb_bt *bt;
	size_t packet_size;
	int error;

	UNUSED_PARAMETER(id);

	/* The controller's record. */
	bt = kern_calloc(1U, sizeof(*bt));
	if (bt == NULL)
		return ENOMEM;
	bt->interface = interface;
	bt->device = drv_usb_interface_device(interface);
	spin_init(&bt->lock, LOCK_RANK_DEVICE, "usb-bt");

	/* The three endpoints the match found. */
	event_in = drv_usb_interface_find_endpoint(interface, DRV_USB_TRANSFER_INTERRUPT, DRV_USB_DIR_IN, NULL);
	data_in = drv_usb_interface_find_endpoint(interface, DRV_USB_TRANSFER_BULK, DRV_USB_DIR_IN, NULL);
	bt->bulk_out = drv_usb_interface_find_endpoint(interface, DRV_USB_TRANSFER_BULK, DRV_USB_DIR_OUT, NULL);
	if (event_in == NULL ||
	    data_in == NULL ||
	    bt->bulk_out == NULL) {
		kern_free(bt);
		return ENODEV;
	}

	/* The events' pipe reads one USB packet a transfer, so an event's end is never waited past. */
	packet_size = drv_usb_endpoint_max_packet_size(event_in) & USB_BT_PACKET_SIZE_MASK;
	if (packet_size == 0U) {
		kern_free(bt);
		return ENODEV;
	}

	/* The events' pipe's transfer and assembler. */
	error = usb_bt_pipe_prepare(bt, &bt->pipes[USB_BT_PIPE_EVENT], event_in, packet_size, BT_HCI_ASSEMBLE_EVENT);
	if (error != 0) {
		usb_bt_free(bt);
		return error;
	}

	/* The work its completion asks for, and its name in the log. */
	bt->pipes[USB_BT_PIPE_EVENT].done_work = USB_BT_WORK_EVENT_DONE;
	bt->pipes[USB_BT_PIPE_EVENT].name = "event";

	/* The ACL packets' pipe. */
	error = usb_bt_pipe_prepare(bt, &bt->pipes[USB_BT_PIPE_DATA], data_in, USB_BT_BULK_BUFFER, BT_HCI_ASSEMBLE_ACL);
	if (error != 0) {
		usb_bt_free(bt);
		return error;
	}

	/* The work its completion asks for, and its name in the log. */
	bt->pipes[USB_BT_PIPE_DATA].done_work = USB_BT_WORK_DATA_DONE;
	bt->pipes[USB_BT_PIPE_DATA].name = "data";

	/* The buffer every packet sent is copied into (a caller's buffer may be on its stack). */
	bt->out = kern_malloc(BT_ACL_PACKET_MAX);
	if (bt->out == NULL) {
		usb_bt_free(bt);
		return ENOMEM;
	}

	/* The product's name and the place. */
	usb_bt_identity(bt);

	/* The isochronous interface, claimed and left at alternate 0 (ws197-p007a2 A1). */
	bt_hci_fifo_init(&bt->sco_queue, bt->sco_queue_bytes, sizeof(bt->sco_queue_bytes));
	usb_bt_sco_claim(bt);

	/* The node, published through the class. */
	descriptor = drv_usb_device_descriptor(bt->device);
	kern_memset(&description, 0, sizeof(description));
	description.vendor = descriptor->vendor;
	description.product = descriptor->product;
	description.version = descriptor->device_release;
	description.bus = BT_BUS_USB;
	description.name = bt->name;
	description.physical_path = bt->physical_path;
	if (bt->sco_interface != NULL)
		description.flags = BT_INFO_SCO;
	error = drv_bt_hci_register(&description, &usb_bt_ops, bt, &bt->hci);
	if (error != 0) {
		usb_bt_free(bt);
		return error;
	}

	/* The record the detach finds. */
	error = drv_usb_interface_set_driver_data(interface, bt);
	if (error != 0) {
		drv_bt_hci_withdraw(bt->hci);
		drv_bt_hci_release(bt->hci);
		usb_bt_free(bt);
		return error;
	}

	/* The worker, whose first request is to submit the transfers. */
	bt->work = USB_BT_WORK_ARM;
	error = kthread_create(usb_bt_worker, bt, SCHED_PRIORITY_DEFAULT, &bt->worker);
	if (error != 0) {
		(void)drv_usb_interface_set_driver_data(interface, NULL);
		drv_bt_hci_withdraw(bt->hci);
		drv_bt_hci_release(bt->hci);
		usb_bt_free(bt);
		return error;
	}

	/* The worker runs. */
	thread_start(bt->worker);

	/* Succeeded: the controller is in service (the SCO interface's alternates told, ws197-p007a0). */
	kern_logf("usb-bt: %s at %s: events %u bytes a transfer\n", bt->name, bt->physical_path, (unsigned)packet_size);
	usb_bt_log_sco(bt, interface);
	return 0;
}

/*
 * Tells the controller's isochronous (SCO) interface, the next after the
 * one taken: each alternate's endpoints and their packet sizes, one line
 * an alternate (ws197-p007a0: the 5330's values before the SCO's design
 * is built on them).  Nothing is claimed or set.
 */
static void
usb_bt_log_sco(
	const struct usb_bt *bt,
	struct drv_usb_interface *interface)
{
	const struct drv_usb_host_interface *alternate;
	const struct drv_usb_interface_descriptor *descriptor;
	const struct drv_usb_endpoint_descriptor *endpoint;
	struct drv_usb_configuration *configuration;
	struct drv_usb_interface *sco;
	char line[160];
	size_t used;
	unsigned count;
	unsigned index;
	unsigned point;
	unsigned points;
	int written;

	/* The interface after this one in the active configuration. */
	configuration = drv_usb_device_active_configuration(drv_usb_interface_device(interface));
	sco = NULL;
	if (configuration != NULL)
		sco = drv_usb_configuration_find_interface(configuration, drv_usb_interface_number(interface) + 1U);
	if (sco == NULL) {
		kern_logf("usb-bt: %s: no SCO interface\n", bt->name);
		return;
	}

	/* Each alternate: its setting, then each endpoint's address and packet size. */
	count = drv_usb_interface_alternate_count(sco);
	for (index = 0U; index < count; index++) {
		alternate = drv_usb_interface_alternate(sco, index);
		if (alternate == NULL)
			continue;
		descriptor = drv_usb_host_interface_descriptor(alternate);
		used = 0U;
		line[0] = '\0';
		points = drv_usb_host_interface_endpoint_count(alternate);
		for (point = 0U; point < points; point++) {
			endpoint = drv_usb_endpoint_descriptor(drv_usb_host_interface_endpoint(alternate, point));
			written = kern_snprintf(line + used, sizeof(line) - used, " 0x%02x/%u/%u", (unsigned)endpoint->address,
			    (unsigned)(endpoint->maximum_packet_size & 0x7ffU), (unsigned)endpoint->interval);
			if (written < 0 || (size_t)written >= sizeof(line) - used)
				break;
			used += (size_t)written;
		}

		/* The alternate's line. */
		kern_logf("usb-bt: %s: SCO interface %u alternate %u endpoints%s\n", bt->name, (unsigned)descriptor->interface_number,
		    (unsigned)descriptor->alternate_setting, line);
	}
}

/* Gives the controller up: the class's operations stop, then the transfers and the worker, then the node. */
static int
usb_bt_detach(
	struct drv_usb_interface *interface,
	unsigned flags)
{
	struct usb_bt *bt;
	int drain_error;
	int join_error;
	int sco_error;

	UNUSED_PARAMETER(flags);

	/* The controller the interface had. */
	bt = drv_usb_interface_driver_data(interface);
	if (bt == NULL)
		return 0;

	/* No operation of the class runs from now on (again harmlessly when the core retries the detach). */
	drv_bt_hci_withdraw(bt->hci);

	/* The SCO stream first (A7), then the transfers stop and the worker leaves. */
	sco_error = usb_bt_sco_stop(bt);
	drain_error = usb_bt_stop_transfers(bt);
	join_error = usb_bt_join_worker(bt);
	if (drain_error == 0)
		drain_error = sco_error;

	/*
	 * A transfer that did not retire may still be the host controller's:
	 * nothing is freed, and the core tries the detach again later
	 * (DETACH_PENDING), as usb-hid does.
	 */
	if (drain_error != 0)
		return drain_error;

	/* A worker that did not end is waited for at the next try too. */
	if (join_error != 0)
		return join_error;

	/* The isochronous interface is given back. */
	if (bt->sco_interface != NULL) {
		(void)drv_usb_interface_release(interface, bt->sco_interface);
		bt->sco_interface = NULL;
	}

	/* The node goes, then the record. */
	drv_bt_hci_release(bt->hci);
	bt->hci = NULL;
	(void)drv_usb_interface_set_driver_data(interface, NULL);
	usb_bt_free(bt);

	/* Succeeded: the controller is given up. */
	return 0;
}

/*
 * Prepares an IN pipe: its transfer, the buffer the transfer fills, and
 * the assembler with its buffer for the longest packet of the mode.
 */
static int
usb_bt_pipe_prepare(
	struct usb_bt *bt,
	struct usb_bt_pipe *pipe,
	struct drv_usb_endpoint *endpoint,
	size_t buffer_size,
	unsigned mode)
{
	size_t capacity;

	/* The endpoint and its transfer. */
	pipe->endpoint = endpoint;
	pipe->urb = drv_usb_urb_alloc(bt->device, endpoint, 0U);
	if (pipe->urb == NULL)
		return ENOMEM;

	/* The buffer the transfer fills. */
	pipe->buffer = kern_malloc(buffer_size);
	if (pipe->buffer == NULL)
		return ENOMEM;
	pipe->buffer_size = buffer_size;

	/* The assembler's buffer holds the longest packet either mode reads (the bulk pipe changes mode). */
	capacity = bt_hci_assembly_capacity(BT_HCI_ASSEMBLE_ACL);
	pipe->assembly = kern_malloc(capacity);
	if (pipe->assembly == NULL)
		return ENOMEM;
	bt_hci_assembler_init(&pipe->assembler, pipe->assembly, capacity, mode, BT_ACL_DATA_MAX);

	/* Succeeded: the pipe is ready to be submitted. */
	return 0;
}

/* Frees an IN pipe's transfer and buffers. */
static void
usb_bt_pipe_free(
	struct usb_bt_pipe *pipe)
{
	/* The transfer, when it was allocated. */
	if (pipe->urb != NULL)
		drv_usb_urb_free(pipe->urb);
	pipe->urb = NULL;

	/* The buffers. */
	if (pipe->buffer != NULL)
		kern_free(pipe->buffer);
	if (pipe->assembly != NULL)
		kern_free(pipe->assembly);
	pipe->buffer = NULL;
	pipe->assembly = NULL;
}

/* Frees the controller's pipes, its send buffer and its record. */
static void
usb_bt_free(
	struct usb_bt *bt)
{
	unsigned index;

	/* Each pipe. */
	for (index = 0U; index < USB_BT_PIPES; index++)
		usb_bt_pipe_free(&bt->pipes[index]);

	/* The send buffer and the record. */
	if (bt->out != NULL)
		kern_free(bt->out);
	kern_free(bt);
}

/* Builds the product's name and the place the controller is presented under. */
static void
usb_bt_identity(
	struct usb_bt *bt)
{
	const struct drv_usb_device_descriptor *descriptor;
	struct drv_usb_bus *usb_bus;
	unsigned bus;
	unsigned port;
	unsigned address;
	unsigned interface_number;

	/* The topology as the place, as usb-hid writes it. */
	usb_bus = drv_usb_device_bus(bt->device);
	bus = drv_usb_bus_number(usb_bus);
	port = drv_usb_device_port(bt->device);
	address = drv_usb_device_address(bt->device);
	interface_number = drv_usb_interface_number(bt->interface);
	kern_snprintf(bt->physical_path, sizeof(bt->physical_path), "usb%u/port%u/device%u/interface%u", bus, port,
		      address, interface_number);

	/* The product's name, or a plain one. */
	descriptor = drv_usb_device_descriptor(bt->device);
	kern_snprintf(bt->name, sizeof(bt->name), "USB Bluetooth controller");
	if (descriptor->product_string != 0U)
		(void)drv_usb_device_get_string(bt->device, descriptor->product_string, 0, bt->name, sizeof(bt->name));
}

/*
 * Stops the transfers for good (the detach): the worker submits no more,
 * a submitted one is cancelled, and each is drained.  Returns 0, or the
 * error of a transfer that did not retire (the first one).
 */
static int
usb_bt_stop_transfers(
	struct usb_bt *bt)
{
	enum drv_usb_urb_status status;
	unsigned long irq;
	unsigned submitting;
	unsigned index;
	int first_error;
	int error;

	/* stopping tells the worker to leave and admits no more submissions. */
	irq = spin_lock_irqsave(&bt->lock);

	bt->stopping = 1U;

	spin_unlock_irqrestore(&bt->lock, irq);

	/* A submission admitted just before is waited out, so its transfer is the one cancelled below. */
	for (;;) {
		irq = spin_lock_irqsave(&bt->lock);

		submitting = bt->submitting;

		spin_unlock_irqrestore(&bt->lock, irq);

		/* None under way: every transfer that will ever be submitted is. */
		if (submitting == 0U)
			break;
		sched_yield();
	}

	/* Each pipe's transfer is cancelled while submitted, then drained. */
	first_error = 0;
	for (index = 0U; index < USB_BT_PIPES; index++) {
		status = drv_usb_urb_status(bt->pipes[index].urb);
		if (status == DRV_USB_URB_PENDING)
			(void)drv_usb_urb_cancel(bt->pipes[index].urb);

		/* The transfer retires; one that does not is logged and keeps the record alive. */
		error = drv_usb_urb_drain(bt->pipes[index].urb, USB_BT_DRAIN_MS);
		if (error != 0) {
			kern_logf("usb-bt: %s: the %s transfer did not retire (%d)\n", bt->name, bt->pipes[index].name, error);
			if (first_error == 0)
				first_error = error;
		}
	}

	/* Reports a transfer that is still the host controller's. */
	if (first_error != 0)
		return first_error;

	/* Succeeded: no transfer is submitted. */
	return 0;
}

/* Wakes the worker to leave and waits for it to end. */
static int
usb_bt_join_worker(
	struct usb_bt *bt)
{
	struct thread *worker;
	unsigned state;
	int error;

	/* No worker was started. */
	worker = bt->worker;
	if (worker == NULL)
		return 0;

	/* The worker cannot wait for itself. */
	if (worker == curthread)
		return EBUSY;

	/* Wakes it; it sees stopping and leaves. */
	kern_thread_wakeup(worker);

	/* Yields until it has ended (a zombie waits for thread_wait). */
	for (;;) {
		state = atomic_raw_load_acquire((volatile unsigned *)&worker->state);
		if (state == THREAD_ZOMBIE)
			break;
		sched_yield();
	}

	/* Its end is collected. */
	error = thread_wait(worker, NULL);
	if (error != 0)
		return error;
	bt->worker = NULL;

	/* Succeeded: the worker is gone. */
	return 0;
}

/* Takes one finished IN transfer: marks its pipe done and wakes the worker (any context). */
static void
usb_bt_completion(
	struct drv_usb_urb *urb,
	void *argument)
{
	struct usb_bt *bt;
	unsigned index;

	/* The pipe of the transfer. */
	bt = argument;
	for (index = 0U; index < USB_BT_PIPES; index++) {
		if (bt->pipes[index].urb == urb) {
			usb_bt_request(bt, bt->pipes[index].done_work);
			break;
		}
	}
}

/* Asks the worker for work and wakes it (any context; it does not sleep). */
static void
usb_bt_request(
	struct usb_bt *bt,
	unsigned work)
{
	struct thread *worker;
	unsigned long irq;

	/* The request, and the worker to wake. */
	irq = spin_lock_irqsave(&bt->lock);

	bt->work |= work;
	worker = bt->worker;

	spin_unlock_irqrestore(&bt->lock, irq);

	/* A worker not started yet finds the request when it starts. */
	if (worker != NULL)
		kern_thread_wakeup(worker);
}

/*
 * Runs the controller's reception: finishes the transfers that ended,
 * hands their packets to the class, and submits each transfer again when
 * its bytes are all taken; sets the transfers aside for the reset.
 */
static void
usb_bt_worker(
	void *argument)
{
	struct usb_bt *bt;
	unsigned long irq;
	unsigned stopping;
	unsigned paused;
	unsigned index;
	unsigned work;
	int flushed;

	/* Until the detach. */
	bt = argument;
	for (;;) {
		/* The requests made since the last round. */
		irq = spin_lock_irqsave(&bt->lock);

		work = bt->work;
		bt->work = 0U;
		paused = bt->paused;
		stopping = bt->stopping;

		spin_unlock_irqrestore(&bt->lock, irq);

		/* The detach: the transfers are its to stop. */
		if (stopping)
			return;

		/* Nothing to do: sleeps until a request (one made just before is kept). */
		if (work == 0U) {
			kern_thread_block();
			continue;
		}

		/* A change of the bootloader's path reads the bulk pipe anew. */
		if ((work & USB_BT_WORK_MODE) != 0U)
			usb_bt_change_mode(bt);

		/* The transfers that ended. */
		for (index = 0U; index < USB_BT_PIPES; index++) {
			if ((work & bt->pipes[index].done_work) != 0U)
				usb_bt_finish(bt, &bt->pipes[index]);
		}

		/* The reset is waiting: the transfers are set aside instead of submitted. */
		if (paused) {
			usb_bt_set_aside(bt);
			continue;
		}

		/* Each pipe hands on what it holds, and is submitted again once it holds nothing. */
		for (index = 0U; index < USB_BT_PIPES; index++) {
			flushed = usb_bt_flush(bt, &bt->pipes[index]);
			if (flushed)
				usb_bt_arm(bt, &bt->pipes[index]);
		}
	}
}

/*
 * Finishes one ended transfer: drains it, then keeps its bytes for the
 * class, clears a stalled endpoint, or counts a failure.
 */
static void
usb_bt_finish(
	struct usb_bt *bt,
	struct usb_bt_pipe *pipe)
{
	enum drv_usb_urb_status status;
	unsigned long irq;
	unsigned paused;
	size_t actual;
	int error;

	/* The transfer retires before its buffer is read. */
	error = drv_usb_urb_drain(pipe->urb, USB_BT_DRAIN_MS);
	if (error != 0) {
		usb_bt_give_up(bt, pipe, "drain", error);
		return;
	}

	/* How it ended, and how many bytes came. */
	status = drv_usb_urb_status(pipe->urb);
	actual = drv_usb_urb_actual_length(pipe->urb);

	/* The pipe is no longer submitted. */
	irq = spin_lock_irqsave(&bt->lock);

	pipe->armed = 0U;
	paused = bt->paused;

	spin_unlock_irqrestore(&bt->lock, irq);

	/* What the transfer ended with. */
	switch (status) {
	case DRV_USB_URB_COMPLETE:
		/* The bytes wait to be cut into packets, unless the reset is about to drop them. */
		pipe->errors = 0U;
		if (!paused) {
			pipe->held_offset = 0U;
			pipe->held_length = actual;
			if (actual > pipe->buffer_size)
				pipe->held_length = pipe->buffer_size;
		}

		/* The bytes are held. */
		break;
	case DRV_USB_URB_STALL:
		/* A stall counts as a failure, so that an endpoint that stalls every time is given up. */
		pipe->errors++;
		if (pipe->errors >= USB_BT_ERRORS_MAX) {
			usb_bt_give_up(bt, pipe, "stall", (int)status);
			break;
		}

		/* A stalled endpoint is cleared and submitted again. */
		error = drv_usb_endpoint_clear_halt(pipe->endpoint);
		if (error != 0)
			usb_bt_give_up(bt, pipe, "clear halt", error);
		break;
	case DRV_USB_URB_CANCELLED:
		/* Cancelled by the reset or the detach, which decide what comes next. */
		break;
	case DRV_USB_URB_DISCONNECTED:
		/* The controller is gone; the detach follows. */
		usb_bt_give_up(bt, pipe, "transfer", (int)status);
		break;
	case DRV_USB_URB_IO_ERROR:
		/*
		 * A transaction error halts the endpoint in the host controller,
		 * which takes no transfer until the endpoint is reset (BUG-287:
		 * one error stopped every ACL packet).  It is cleared as a stall
		 * is, up to the same limit.
		 */
		pipe->errors++;
		if (pipe->errors >= USB_BT_ERRORS_MAX) {
			usb_bt_give_up(bt, pipe, "transfer", (int)status);
			break;
		}

		/* The endpoint cleared on both sides, and submitted again. */
		error = drv_usb_endpoint_clear_halt(pipe->endpoint);
		if (error != 0) {
			usb_bt_give_up(bt, pipe, "clear halt", error);
			break;
		}

		/* Logged: the pipe goes on. */
		kern_logf("usb-bt: %s: the %s pipe went on after a transfer error (%u in a row)\n", bt->name, pipe->name, pipe->errors);
		break;
	default:
		/* A failed transfer is tried again, up to a limit. */
		pipe->errors++;
		if (pipe->errors >= USB_BT_ERRORS_MAX)
			usb_bt_give_up(bt, pipe, "transfer", (int)status);
		break;
	}

	/*
	 * Bytes lost to a transfer that did not complete leave a packet half
	 * gathered that no later byte belongs to: the pipe starts its next
	 * packet afresh.
	 */
	if (status != DRV_USB_URB_COMPLETE)
		usb_bt_pipe_restart(pipe);
}

/*
 * Hands the bytes a pipe holds to the class, packet by packet.  Returns
 * nonzero when the pipe holds nothing more (its transfer may be submitted
 * again), 0 while the class has no room for a packet.
 */
static int
usb_bt_flush(
	struct usb_bt *bt,
	struct usb_bt_pipe *pipe)
{
	struct drv_bt_hci_counts counts;
	size_t remaining;
	size_t consumed;
	int error;

	/* The bytes not yet taken, and a whole packet the class refused before. */
	remaining = pipe->held_length - pipe->held_offset;
	consumed = 0U;
	error = bt_hci_assembler_feed(
		&pipe->assembler,
		pipe->buffer + pipe->held_offset,
		remaining,
		usb_bt_deliver,
		bt,
		&consumed);
	pipe->held_offset += consumed;

	/* The class has no room: the pipe keeps the rest and waits for the room call. */
	kern_memset(&counts, 0, sizeof(counts));
	if (error == ENOSPC) {
		if (!pipe->stalled) {
			pipe->stalled = 1U;
			counts.stalls = 1U;
			drv_bt_hci_count(bt->hci, &counts);
		}

		/* Not a failure: the rest waits for room. */
		return 0;
	}

	/* The class took what it could: the pipe is not held back. */
	pipe->stalled = 0U;

	/* A malformed transfer: its rest is dropped and the next one starts a packet. */
	if (error != 0) {
		counts.malformed = 1U;
		drv_bt_hci_count(bt->hci, &counts);
	}

	/* Succeeded: the pipe holds nothing. */
	pipe->held_offset = 0U;
	pipe->held_length = 0U;
	return 1;
}

/* Submits a pipe's transfer again, unless it is submitted, given up, paused or stopping. */
static void
usb_bt_arm(
	struct usb_bt *bt,
	struct usb_bt_pipe *pipe)
{
	unsigned long irq;
	int admitted;
	int error;

	/*
	 * armed is taken here, so that the reset sees a transfer about to be
	 * submitted; submitting, so that the detach waits for the submission
	 * before it cancels.
	 */
	admitted = 0;
	irq = spin_lock_irqsave(&bt->lock);

	if (!bt->stopping &&
	    !bt->paused &&
	    !pipe->armed &&
	    !pipe->dead) {
		pipe->armed = 1U;
		bt->submitting++;
		admitted = 1;
	}

	spin_unlock_irqrestore(&bt->lock, irq);

	/* Nothing to submit. */
	if (!admitted)
		return;

	/* The transfer, filling the whole buffer or ending at a short packet, and its submission. */
	error = drv_usb_urb_setup(
		pipe->urb,
		pipe->buffer,
		pipe->buffer_size,
		DRV_USB_URB_SHORT_OK,
		0U,
		usb_bt_completion,
		bt);
	if (error == 0) {
		/* Set up: submitted. */
		error = drv_usb_urb_submit(pipe->urb);
	}

	/* The submission is over, submitted or not. */
	irq = spin_lock_irqsave(&bt->lock);

	bt->submitting--;

	spin_unlock_irqrestore(&bt->lock, irq);

	/* A transfer that could not be submitted gives the pipe up. */
	if (error != 0) {
		irq = spin_lock_irqsave(&bt->lock);

		pipe->armed = 0U;

		spin_unlock_irqrestore(&bt->lock, irq);

		/* Logged and stopped until the reset. */
		usb_bt_give_up(bt, pipe, "submit", error);
	}
}

/* Gives a pipe up after a failure it cannot recover from, and logs why. */
static void
usb_bt_give_up(
	struct usb_bt *bt,
	struct usb_bt_pipe *pipe,
	const char *stage,
	int error)
{
	unsigned long irq;
	unsigned was_dead;

	/* dead keeps the pipe from being submitted again until the reset. */
	irq = spin_lock_irqsave(&bt->lock);

	was_dead = pipe->dead;
	pipe->dead = 1U;
	pipe->armed = 0U;

	spin_unlock_irqrestore(&bt->lock, irq);

	/* Logged once, not at every later failure. */
	if (!was_dead)
		kern_logf("usb-bt: %s: the %s pipe stopped at %s (%d)\n", bt->name, pipe->name, stage, error);
}

/*
 * Sets the transfers aside for the reset: once neither is submitted, the
 * bytes held and the packets half gathered are dropped, and the reset is
 * told.
 */
static void
usb_bt_set_aside(
	struct usb_bt *bt)
{
	unsigned long irq;
	unsigned index;
	unsigned armed;

	/* Whether a transfer is still submitted. */
	irq = spin_lock_irqsave(&bt->lock);

	armed = 0U;
	for (index = 0U; index < USB_BT_PIPES; index++)
		armed |= bt->pipes[index].armed;

	spin_unlock_irqrestore(&bt->lock, irq);

	/* A transfer still submitted ends (cancelled) and brings the worker back here. */
	if (armed)
		return;

	/* Nothing from before the reset is handed on. */
	for (index = 0U; index < USB_BT_PIPES; index++) {
		usb_bt_pipe_restart(&bt->pipes[index]);
		bt->pipes[index].stalled = 0U;
		bt->pipes[index].errors = 0U;
	}

	/* quiesced tells the waiting reset that the device may be reset. */
	irq = spin_lock_irqsave(&bt->lock);

	bt->quiesced = 1U;

	spin_unlock_irqrestore(&bt->lock, irq);
}

/*
 * Reads the bulk pipe in the path the class set: events while the
 * bootloader's path is on, ACL packets otherwise.  A packet half gathered
 * and the bytes held from before the change are dropped.
 */
static void
usb_bt_change_mode(
	struct usb_bt *bt)
{
	struct usb_bt_pipe *pipe;
	unsigned long irq;
	unsigned bootloader;
	unsigned mode;

	/* The path now. */
	irq = spin_lock_irqsave(&bt->lock);

	bootloader = bt->bootloader;

	spin_unlock_irqrestore(&bt->lock, irq);

	/* The bulk pipe starts afresh in the path's mode. */
	pipe = &bt->pipes[USB_BT_PIPE_DATA];
	mode = BT_HCI_ASSEMBLE_ACL;
	if (bootloader)
		mode = BT_HCI_ASSEMBLE_EVENT;
	pipe->held_offset = 0U;
	pipe->held_length = 0U;
	bt_hci_assembler_restart(&pipe->assembler, mode, BT_ACL_DATA_MAX);
}

/* Hands one packet the assembler put together to the class (ENOSPC when its queue is full). */
static int
usb_bt_deliver(
	void *context,
	uint8_t type,
	const uint8_t *packet,
	size_t length)
{
	struct usb_bt *bt;
	int error;

	/* The class queues it for the reader. */
	bt = context;
	error = drv_bt_hci_input(bt->hci, type, packet, length);
	if (error != 0)
		return error;

	/* Succeeded: the packet is the class's. */
	return 0;
}

/*
 * Sends one checked H4 packet (the class's operation): a command as the
 * class request on the control pipe, an ACL packet, or the bootloader's
 * Secure Send, on bulk OUT.
 */
static int
usb_bt_send(
	void *context,
	const uint8_t *packet,
	size_t length)
{
	struct usb_bt *bt;
	unsigned long irq;
	unsigned bootloader;
	size_t body;
	size_t actual;
	int secure_send;
	int error;

	/* The packet without its type's byte, in the transport's own buffer. */
	bt = context;
	if (length < 2U || length > BT_ACL_PACKET_MAX)
		return EINVAL;
	body = length - 1U;
	kern_memcpy(bt->out, packet + 1, body);

	/* The path now. */
	irq = spin_lock_irqsave(&bt->lock);

	bootloader = bt->bootloader;

	spin_unlock_irqrestore(&bt->lock, irq);

	/* The bootloader takes Secure Send on bulk OUT. */
	secure_send = 0;
	if (bootloader)
		secure_send = bt_hci_is_secure_send(packet, length);

	/* A command on the control pipe: class request 0 to the device. */
	actual = 0U;
	if (packet[0] == BT_PACKET_COMMAND && !secure_send) {
		error = drv_usb_control(bt->device,
		    DRV_USB_DIR_OUT | DRV_USB_REQUEST_CLASS | DRV_USB_RECIP_DEVICE,
		    0U,
		    0U,
		    0U,
		    bt->out,
		    body,
		    USB_BT_COMMAND_MS,
		    &actual);
	} else {
		error = drv_usb_bulk(bt->device, bt->bulk_out, bt->out, body, USB_BT_ACL_MS, &actual);
	}

	/* Reports a transfer that failed. */
	if (error != 0)
		return error;

	/* Reports a transfer the controller took only part of. */
	if (actual != body)
		return EIO;

	/* Succeeded: the controller took the packet. */
	return 0;
}

/* Turns the Intel bootloader's path on or off (the class's operation). */
static int
usb_bt_set_bootloader(
	void *context,
	int on)
{
	struct usb_bt *bt;
	unsigned long irq;

	/* The path the sends and the worker follow from now on. */
	bt = context;
	irq = spin_lock_irqsave(&bt->lock);

	bt->bootloader = 0U;
	if (on)
		bt->bootloader = 1U;

	spin_unlock_irqrestore(&bt->lock, irq);

	/* The worker reads the bulk pipe in the new mode. */
	usb_bt_request(bt, USB_BT_WORK_MODE);

	/* Succeeded: the path is set. */
	return 0;
}

/*
 * Resets the controller in place (the class's operation): sets the
 * transfers aside, resets the USB device, starts the transfers again and
 * queues the RESET notice.
 */
static int
usb_bt_reset(
	void *context)
{
	struct usb_bt *bt;
	unsigned long irq;
	unsigned tries;
	unsigned index;
	int error;

	/* The SCO stream stops first (A7: bluetoothd sets its links again), then nothing is received while the device resets. */
	bt = context;
	error = usb_bt_sco_stop(bt);
	if (error == 0)
		error = usb_bt_sco_select(bt, 0U);
	if (error == 0)
		error = usb_bt_quiesce(bt);

	/* The USB device's reset, tried again while the core is busy. */
	if (error == 0) {
		for (tries = 0U; tries < USB_BT_RESET_TRIES; tries++) {
			error = drv_usb_device_reset(bt->device);
			if (error != EBUSY)
				break;
			usb_bt_sleep_ms(USB_BT_RESET_RETRY_MS);
		}
	}

	/*
	 * The program reads that the controller was reset after everything
	 * from before and before anything from after: the notice is queued
	 * while the transfers are still set aside.
	 */
	if (error == 0)
		drv_bt_hci_notice(bt->hci, BT_PACKET_NOTICE_RESET);

	/* The pipes start again (a pipe given up before is tried anew) and the worker submits them. */
	irq = spin_lock_irqsave(&bt->lock);

	bt->paused = 0U;
	bt->quiesced = 0U;
	for (index = 0U; index < USB_BT_PIPES; index++)
		bt->pipes[index].dead = 0U;

	spin_unlock_irqrestore(&bt->lock, irq);

	/* The worker submits the transfers. */
	usb_bt_request(bt, USB_BT_WORK_ARM);

	/* Reports why the controller was not reset. */
	if (error != 0)
		return error;

	/* Succeeded: the controller was reset. */
	return 0;
}

/* Tells the worker that a read has made room (the class's operation; it does not sleep). */
static void
usb_bt_room(
	void *context)
{
	/* The worker tries the pipes that hold packets again. */
	usb_bt_request(context, USB_BT_WORK_ROOM);
}

/*
 * Pauses the reception for the reset: the worker submits nothing, the
 * submitted transfers are cancelled, and the worker drops what it holds.
 * Returns 0, or ETIMEDOUT when the worker did not set the transfers aside.
 */
static int
usb_bt_quiesce(
	struct usb_bt *bt)
{
	enum drv_usb_urb_status status;
	unsigned long irq;
	unsigned waited;
	unsigned quiesced;
	unsigned index;

	/* paused keeps the worker from submitting; it sets the transfers aside. */
	irq = spin_lock_irqsave(&bt->lock);

	bt->paused = 1U;
	bt->quiesced = 0U;

	spin_unlock_irqrestore(&bt->lock, irq);

	/* The worker sets the transfers aside. */
	usb_bt_request(bt, USB_BT_WORK_QUIESCE);

	/*
	 * Waits for the worker's answer.  Each round cancels a submitted
	 * transfer, which ends and brings the worker round; a transfer the
	 * worker was submitting as paused was set is caught by a later round.
	 */
	for (waited = 0U; waited < USB_BT_QUIESCE_MS; waited += USB_BT_QUIESCE_STEP_MS) {
		for (index = 0U; index < USB_BT_PIPES; index++) {
			status = drv_usb_urb_status(bt->pipes[index].urb);
			if (status == DRV_USB_URB_PENDING)
				(void)drv_usb_urb_cancel(bt->pipes[index].urb);
		}

		/* The worker's answer. */
		irq = spin_lock_irqsave(&bt->lock);

		quiesced = bt->quiesced;

		spin_unlock_irqrestore(&bt->lock, irq);

		/* The transfers are set aside. */
		if (quiesced)
			return 0;
		usb_bt_sleep_ms(USB_BT_QUIESCE_STEP_MS);
	}

	/* The worker did not answer in time. */
	return ETIMEDOUT;
}

/* Sleeps the calling thread for about the milliseconds given. */
static void
usb_bt_sleep_ms(
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

/* Drops what a pipe holds and the packet it half gathered: its next packet starts afresh, in its mode. */
static void
usb_bt_pipe_restart(
	struct usb_bt_pipe *pipe)
{
	/* No bytes held, and the assembler empty, at the receiving limit. */
	pipe->held_offset = 0U;
	pipe->held_length = 0U;
	bt_hci_assembler_restart(&pipe->assembler, pipe->assembler.mode, BT_ACL_DATA_MAX);
}

/*
 * Claims the controller's isochronous interface: the next one, of the
 * same class, whose alternates carry isochronous endpoints only.  Without
 * it the controller has no SCO (A1); alternate 6 tells a transparent
 * link's alternate (S3).
 */
static void
usb_bt_sco_claim(
	struct usb_bt *bt)
{
	const struct drv_usb_interface_descriptor *descriptor;
	struct drv_usb_configuration *configuration;
	struct drv_usb_interface *sco;
	const struct drv_usb_host_interface *transparent;
	int error;

	/* The interface after this one in the active configuration. */
	configuration = drv_usb_device_active_configuration(bt->device);
	if (configuration == NULL)
		return;
	sco = drv_usb_configuration_find_interface(configuration, drv_usb_interface_number(bt->interface) + 1U);
	if (sco == NULL)
		return;

	/* Of the HCI's class. */
	descriptor = drv_usb_interface_descriptor(sco);
	if (descriptor->interface_class != USB_BT_INTERFACE_CLASS ||
	    descriptor->interface_subclass != USB_BT_INTERFACE_SUBCLASS ||
	    descriptor->interface_protocol != USB_BT_INTERFACE_PROTOCOL)
		return;

	/* Claimed for this controller (the core may be probing it: no SCO then). */
	error = drv_usb_interface_claim(bt->interface, sco);
	if (error != 0) {
		kern_logf("usb-bt: %s: SCO interface not claimed (%d)\n", bt->name, error);
		return;
	}

	/* Succeeded: SCO, at alternate 0. */
	transparent = drv_usb_interface_find_alternate(sco, USB_BT_SCO_TRANSPARENT_ALTERNATE);
	bt->sco_interface = sco;
	bt->sco_transparent = transparent != NULL;
	bt->sco_alternate = 0U;
}

/*
 * Sets the SCO links (the class's operation, under its mutex): the
 * stream stops, the alternate the links need is selected, and for links
 * the stream starts again.  The answer carries the alternate and its
 * packets' size.
 */
static int
usb_bt_sco_set(
	void *context,
	const struct bt_sco *links,
	struct bt_sco *answer)
{
	struct usb_bt *bt;
	unsigned alternate;
	int error;

	/* A controller with SCO. */
	bt = context;
	if (bt->sco_interface == NULL)
		return ENOTSUP;

	/* The stream stops; nothing of it is left on the endpoints. */
	error = usb_bt_sco_stop(bt);
	if (error != 0)
		return error;

	/* The alternate the links need. */
	alternate = bt_hci_sco_alternate(links, (int)bt->sco_transparent);
	error = usb_bt_sco_select(bt, alternate);
	if (error != 0)
		return error;

	/* No link: stopped, at alternate 0. */
	answer->alternate = (uint16_t)alternate;
	answer->mtu = 0U;
	if (links->count == 0U)
		return 0;

	/* The stream on the new alternate. */
	error = usb_bt_sco_start(bt, links);
	if (error != 0) {
		(void)usb_bt_sco_select(bt, 0U);
		answer->alternate = 0U;
		return error;
	}

	/* Succeeded: streaming. */
	answer->mtu = (uint16_t)bt->sco_mtu;
	return 0;
}

/*
 * Queues one SCO packet the program wrote (the class's operation; never
 * sleeps): its bytes after the type's, whole, within the queue's limit.
 */
static int
usb_bt_sco_send(
	void *context,
	const uint8_t *packet,
	size_t length)
{
	struct usb_bt *bt;
	struct thread *thread;
	unsigned long irq;
	size_t size;
	int error;

	/* The bytes the pipe carries: the header and the data. */
	bt = context;
	size = length - 1U;

	/* Into the queue, while the stream runs. */
	thread = NULL;
	irq = spin_lock_irqsave(&bt->lock);
	if (!bt->sco_streaming) {
		error = ENOTCONN;
	} else if (bt->sco_queue.used + size > bt->sco_queue_limit) {
		error = EAGAIN;
	} else {
		error = bt_hci_fifo_put(&bt->sco_queue, packet + 1, size);
		bt->sco_work |= USB_BT_SCO_WORK_SEND;
		thread = bt->sco_thread;
	}

	/* Queued or not. */
	spin_unlock_irqrestore(&bt->lock, irq);

	/* The thread fills the OUT transfers. */
	if (thread != NULL)
		kern_thread_wakeup(thread);

	/* Queued, or refused. */
	return error;
}

/*
 * Stops the SCO stream: no more packets are taken, the thread leaves, and
 * every SCO transfer is cancelled (one cancel ends the endpoint's all),
 * drained and freed.  Returns 0, or the error of a transfer that did not
 * retire (the record then stays the host controller's).
 */
static int
usb_bt_sco_stop(
	struct usb_bt *bt)
{
	struct thread *thread;
	struct drv_usb_urb *urb;
	enum drv_usb_urb_status status;
	unsigned long irq;
	unsigned state;
	unsigned index;
	int first_error;
	int error;

	/* No more packets taken, the thread told to leave. */
	irq = spin_lock_irqsave(&bt->lock);
	bt->sco_streaming = 0U;
	bt->sco_stopping = 1U;
	thread = bt->sco_thread;
	spin_unlock_irqrestore(&bt->lock, irq);

	/* The thread leaves; it is waited for asleep, never by spinning (design 2.1b B-P1). */
	if (thread != NULL) {
		kern_thread_wakeup(thread);
		for (;;) {
			state = atomic_raw_load_acquire((volatile unsigned *)&thread->state);
			if (state == THREAD_ZOMBIE)
				break;
			usb_bt_sleep_ms(1U);
		}

		/* Its end collected. */
		(void)thread_wait(thread, NULL);
		bt->sco_thread = NULL;
	}

	/* Each transfer cancelled while submitted, then drained. */
	first_error = 0;
	for (index = 0U; index < USB_BT_SCO_IN_URBS + USB_BT_SCO_OUT_URBS; index++) {
		/* IN first, then OUT. */
		urb = NULL;
		if (index < USB_BT_SCO_IN_URBS)
			urb = bt->sco_in_urbs[index];
		else
			urb = bt->sco_out_urbs[index - USB_BT_SCO_IN_URBS];
		if (urb == NULL)
			continue;
		status = drv_usb_urb_status(urb);
		if (status == DRV_USB_URB_PENDING)
			(void)drv_usb_urb_cancel(urb);

		/* Retired, or kept with the record. */
		error = usb_bt_sco_retire(urb);
		if (error != 0 && first_error == 0)
			first_error = error;
	}

	/* A transfer still the host controller's keeps everything. */
	if (first_error != 0) {
		kern_logf("usb-bt: %s: an SCO transfer did not retire (%d)\n", bt->name, first_error);
		return first_error;
	}

	/* The transfers and their buffers go, the queue empties. */
	usb_bt_sco_free(bt);
	irq = spin_lock_irqsave(&bt->lock);
	bt->sco_stopping = 0U;
	bt->sco_work = 0U;
	bt->sco_in_busy = 0U;
	bt->sco_out_busy = 0U;
	bt_hci_fifo_clear(&bt->sco_queue);
	spin_unlock_irqrestore(&bt->lock, irq);

	/* Succeeded: nothing of the stream is left. */
	return 0;
}

/*
 * Selects an alternate of the isochronous interface (SET_INTERFACE), its
 * transfers already drained.  The default control pipe may be busy with an
 * HCI command: tried again every 5 ms for 50 ms (S7).
 */
static int
usb_bt_sco_select(
	struct usb_bt *bt,
	unsigned alternate)
{
	unsigned tries;
	int error;

	/* Already there, or no SCO. */
	if (bt->sco_interface == NULL || bt->sco_alternate == alternate)
		return 0;

	/* SET_INTERFACE, while the control pipe is busy tried again. */
	error = EBUSY;
	for (tries = 0U; tries < USB_BT_SCO_ALTERNATE_TRIES; tries++) {
		error = drv_usb_interface_set_alternate(bt->sco_interface, alternate);
		if (error != EBUSY)
			break;
		usb_bt_sleep_ms(USB_BT_SCO_ALTERNATE_STEP_MS);
	}

	/* A device the core quarantined fails here and in every later submission (design 2.1a S8). */
	if (error != 0) {
		kern_logf("usb-bt: %s: SCO alternate %u not selected (%d)\n", bt->name, alternate, error);
		return error;
	}

	/* Succeeded: the alternate. */
	bt->sco_alternate = alternate;
	return 0;
}

/*
 * Starts the stream on the selected alternate: its endpoints and their
 * packets' size (IN and OUT the same), the transfers and buffers, the
 * assembler and the queue's limit, the thread, and the IN transfers.
 */
static int
usb_bt_sco_start(
	struct usb_bt *bt,
	const struct bt_sco *links)
{
	unsigned long irq;
	size_t in_size;
	size_t out_size;
	unsigned index;
	int error;

	/* The alternate's endpoints. */
	bt->sco_in = drv_usb_interface_find_endpoint(bt->sco_interface, DRV_USB_TRANSFER_ISOCHRONOUS, DRV_USB_DIR_IN, NULL);
	bt->sco_out = drv_usb_interface_find_endpoint(bt->sco_interface, DRV_USB_TRANSFER_ISOCHRONOUS, DRV_USB_DIR_OUT, NULL);
	if (bt->sco_in == NULL || bt->sco_out == NULL)
		return ENODEV;

	/* Their packets' size, the same both ways (S2). */
	in_size = drv_usb_endpoint_max_packet_size(bt->sco_in) & USB_BT_PACKET_SIZE_MASK;
	out_size = drv_usb_endpoint_max_packet_size(bt->sco_out) & USB_BT_PACKET_SIZE_MASK;
	if (in_size == 0U || in_size != out_size)
		return EINVAL;
	bt->sco_mtu = in_size;

	/* The transfers and their buffers. */
	for (index = 0U; index < USB_BT_SCO_IN_URBS; index++) {
		bt->sco_in_urbs[index] = drv_usb_urb_alloc(bt->device, bt->sco_in, USB_BT_SCO_IN_PACKETS);
		bt->sco_in_buffers[index] = kern_malloc(USB_BT_SCO_IN_PACKETS * in_size);
		if (bt->sco_in_urbs[index] == NULL || bt->sco_in_buffers[index] == NULL) {
			usb_bt_sco_free(bt);
			return ENOMEM;
		}
	}

	/* The OUT ones. */
	for (index = 0U; index < USB_BT_SCO_OUT_URBS; index++) {
		bt->sco_out_urbs[index] = drv_usb_urb_alloc(bt->device, bt->sco_out, USB_BT_SCO_OUT_PACKETS);
		bt->sco_out_buffers[index] = kern_malloc(USB_BT_SCO_OUT_PACKETS * out_size);
		if (bt->sco_out_urbs[index] == NULL || bt->sco_out_buffers[index] == NULL) {
			usb_bt_sco_free(bt);
			return ENOMEM;
		}
	}

	/* The assembler for the links, the queue's limit in the first link's packets. */
	bt_hci_sco_assembler_init(&bt->sco_assembler, links);
	bt->sco_queue_limit = USB_BT_SCO_QUEUE_PACKETS * (BT_SCO_HEADER + (size_t)links->packet_length[0]);
	if (bt->sco_queue_limit > sizeof(bt->sco_queue_bytes))
		bt->sco_queue_limit = sizeof(bt->sco_queue_bytes);
	bt->sco_dropped = 0U;

	/* The thread, above the default priority. */
	error = kthread_create(usb_bt_sco_thread, bt, USB_BT_SCO_PRIORITY, &bt->sco_thread);
	if (error != 0) {
		bt->sco_thread = NULL;
		usb_bt_sco_free(bt);
		return error;
	}

	/* Taking packets, the IN transfers submitted first by the thread. */
	irq = spin_lock_irqsave(&bt->lock);
	bt->sco_streaming = 1U;
	bt->sco_work = (1U << USB_BT_SCO_IN_URBS) - 1U;
	spin_unlock_irqrestore(&bt->lock, irq);
	thread_start(bt->sco_thread);

	/* Succeeded: streaming. */
	return 0;
}

/* Frees the SCO transfers and their buffers (none submitted). */
static void
usb_bt_sco_free(
	struct usb_bt *bt)
{
	unsigned index;

	/* IN. */
	for (index = 0U; index < USB_BT_SCO_IN_URBS; index++) {
		if (bt->sco_in_urbs[index] != NULL)
			drv_usb_urb_free(bt->sco_in_urbs[index]);
		kern_free(bt->sco_in_buffers[index]);
		bt->sco_in_urbs[index] = NULL;
		bt->sco_in_buffers[index] = NULL;
	}

	/* OUT. */
	for (index = 0U; index < USB_BT_SCO_OUT_URBS; index++) {
		if (bt->sco_out_urbs[index] != NULL)
			drv_usb_urb_free(bt->sco_out_urbs[index]);
		kern_free(bt->sco_out_buffers[index]);
		bt->sco_out_urbs[index] = NULL;
		bt->sco_out_buffers[index] = NULL;
	}
}

/* Takes one finished SCO transfer: its bit for the thread, which it wakes (any context; the isochronous contract). */
static void
usb_bt_sco_completion(
	struct drv_usb_urb *urb,
	void *argument)
{
	struct usb_bt *bt;
	struct thread *thread;
	unsigned long irq;
	unsigned index;
	unsigned bit;

	/* The transfer's bit: IN 0 to 2, OUT 8 to 11. */
	bt = argument;
	bit = 0U;
	for (index = 0U; index < USB_BT_SCO_IN_URBS; index++) {
		if (bt->sco_in_urbs[index] == urb)
			bit = 1U << index;
	}

	/* Or an OUT one's. */
	for (index = 0U; index < USB_BT_SCO_OUT_URBS; index++) {
		if (bt->sco_out_urbs[index] == urb)
			bit = 1U << (8U + index);
	}

	/* The request, and the thread woken. */
	irq = spin_lock_irqsave(&bt->lock);
	bt->sco_work |= bit;
	thread = bt->sco_thread;
	spin_unlock_irqrestore(&bt->lock, irq);
	if (thread != NULL)
		kern_thread_wakeup(thread);
}

/*
 * The SCO thread: takes the finished IN transfers' packets and submits
 * them again, and fills the OUT transfers from the queue, until stopped.
 */
static void
usb_bt_sco_thread(
	void *argument)
{
	struct usb_bt *bt;
	unsigned long irq;
	unsigned stopping;
	unsigned work;
	unsigned index;
	uint64_t dropped;
	size_t queued;

	/* Until the stop. */
	bt = argument;
	for (;;) {
		/* The requests since the last round. */
		irq = spin_lock_irqsave(&bt->lock);
		work = bt->sco_work;
		bt->sco_work = 0U;
		stopping = bt->sco_stopping;
		spin_unlock_irqrestore(&bt->lock, irq);

		/* The stop: the transfers are its to cancel. */
		if (stopping)
			return;

		/* Nothing to do: asleep until a completion or a write. */
		if (work == 0U) {
			kern_thread_block();
			continue;
		}

		/* Each IN transfer done (or to start): its packets, then submitted again. */
		for (index = 0U; index < USB_BT_SCO_IN_URBS; index++) {
			if ((work & (1U << index)) == 0U)
				continue;
			usb_bt_sco_receive(bt, index);
			usb_bt_sco_submit_in(bt, index);
		}

		/* The OUT transfers done are free again. */
		irq = spin_lock_irqsave(&bt->lock);
		for (index = 0U; index < USB_BT_SCO_OUT_URBS; index++) {
			if ((work & (1U << (8U + index))) != 0U)
				bt->sco_out_busy &= ~(1U << index);
		}

		/* Marked. */
		spin_unlock_irqrestore(&bt->lock, irq);

		/* The free OUT transfers filled from the queue. */
		usb_bt_sco_fill(bt);

		/* The counts for BT_IOC_GET_STATS. */
		dropped = bt->sco_assembler.resyncs - bt->sco_dropped;
		bt->sco_dropped = bt->sco_assembler.resyncs;
		irq = spin_lock_irqsave(&bt->lock);
		queued = bt->sco_queue.used;
		spin_unlock_irqrestore(&bt->lock, irq);
		drv_bt_hci_sco_count(bt->hci, dropped, queued);
	}
}

/* Puts one finished IN transfer's packets into SCO packets (a missed or failed packet is a gap). */
static void
usb_bt_sco_receive(
	struct usb_bt *bt,
	unsigned index)
{
	struct drv_usb_urb *urb;
	enum drv_usb_urb_status status;
	enum drv_usb_iso_status result;
	unsigned long irq;
	unsigned submitted;
	unsigned packet;
	size_t actual;
	size_t offset;
	size_t length;
	int error;

	/* Only a transfer that was submitted and has ended. */
	urb = bt->sco_in_urbs[index];
	irq = spin_lock_irqsave(&bt->lock);
	submitted = (bt->sco_in_busy & (1U << index)) != 0U;
	bt->sco_in_busy &= ~(1U << index);
	spin_unlock_irqrestore(&bt->lock, irq);
	if (!submitted)
		return;

	/* It retires before its bytes are read (waited for asleep, B-P1). */
	error = usb_bt_sco_retire(urb);
	if (error != 0)
		return;
	status = drv_usb_urb_status(urb);
	if (status != DRV_USB_URB_COMPLETE) {
		bt_hci_sco_gap(&bt->sco_assembler);
		return;
	}

	/* Each packet: its bytes, or a gap. */
	for (packet = 0U; packet < USB_BT_SCO_IN_PACKETS; packet++) {
		error = drv_usb_urb_iso_result(urb, packet, &actual, &result);
		if (error != 0)
			continue;
		if (result != DRV_USB_ISO_OK && result != DRV_USB_ISO_SHORT) {
			bt_hci_sco_gap(&bt->sco_assembler);
			continue;
		}

		/* The bytes, into packets for the class. */
		(void)drv_usb_urb_iso_packet(urb, packet, &offset, &length);
		bt_hci_sco_feed(&bt->sco_assembler, bt->sco_in_buffers[index] + offset, actual, usb_bt_sco_deliver, bt);
	}
}

/* Submits one IN transfer: 8 packets of the alternate's size. */
static void
usb_bt_sco_submit_in(
	struct usb_bt *bt,
	unsigned index)
{
	struct drv_usb_iso_packet packets[USB_BT_SCO_IN_PACKETS];
	struct drv_usb_urb *urb;
	unsigned long irq;
	unsigned packet;
	unsigned streaming;
	int error;

	/* Only while streaming. */
	irq = spin_lock_irqsave(&bt->lock);
	streaming = bt->sco_streaming && !bt->sco_stopping;
	spin_unlock_irqrestore(&bt->lock, irq);
	if (!streaming)
		return;

	/* The packets, one after another in the buffer. */
	urb = bt->sco_in_urbs[index];
	kern_memset(packets, 0, sizeof(packets));
	for (packet = 0U; packet < USB_BT_SCO_IN_PACKETS; packet++) {
		packets[packet].offset = packet * bt->sco_mtu;
		packets[packet].length = bt->sco_mtu;
	}

	/* Set up and submitted; a refusal is a gap (the transfer is tried again at the next round). */
	error = drv_usb_urb_setup(urb, bt->sco_in_buffers[index], USB_BT_SCO_IN_PACKETS * bt->sco_mtu, 0U, 0U,
				  usb_bt_sco_completion, bt);
	if (error == 0)
		error = drv_usb_urb_setup_isochronous(urb, packets, USB_BT_SCO_IN_PACKETS);
	if (error == 0)
		error = drv_usb_urb_submit(urb);
	if (error != 0) {
		bt_hci_sco_gap(&bt->sco_assembler);
		return;
	}

	/* Submitted. */
	irq = spin_lock_irqsave(&bt->lock);
	bt->sco_in_busy |= 1U << index;
	spin_unlock_irqrestore(&bt->lock, irq);
}

/*
 * Fills the free OUT transfers from the send queue, four packets of the
 * alternate's size each, while it holds that much (the SCO packets are a
 * stream of bytes across the isochronous packets).
 */
static void
usb_bt_sco_fill(
	struct usb_bt *bt)
{
	struct drv_usb_iso_packet packets[USB_BT_SCO_OUT_PACKETS];
	struct drv_usb_urb *urb;
	unsigned long irq;
	unsigned index;
	unsigned packet;
	unsigned free_bit;
	size_t whole;
	size_t got;
	int error;

	/* Each free transfer while the queue holds a whole transfer's bytes. */
	whole = USB_BT_SCO_OUT_PACKETS * bt->sco_mtu;
	for (index = 0U; index < USB_BT_SCO_OUT_URBS; index++) {
		/* Taken from the queue under the lock. */
		got = 0U;
		irq = spin_lock_irqsave(&bt->lock);
		free_bit = (bt->sco_out_busy & (1U << index)) == 0U;
		if (free_bit && bt->sco_streaming && bt->sco_queue.used >= whole) {
			got = bt_hci_fifo_get(&bt->sco_queue, bt->sco_out_buffers[index], whole);
			bt->sco_out_busy |= 1U << index;
		}

		/* Nothing for this one. */
		spin_unlock_irqrestore(&bt->lock, irq);
		if (got == 0U)
			continue;

		/* Four packets of the alternate's size. */
		urb = bt->sco_out_urbs[index];
		kern_memset(packets, 0, sizeof(packets));
		for (packet = 0U; packet < USB_BT_SCO_OUT_PACKETS; packet++) {
			packets[packet].offset = packet * bt->sco_mtu;
			packets[packet].length = bt->sco_mtu;
		}

		/* Set up and submitted; refused, the bytes are lost and the transfer free again. */
		error = drv_usb_urb_setup(urb, bt->sco_out_buffers[index], whole, 0U, 0U, usb_bt_sco_completion, bt);
		if (error == 0)
			error = drv_usb_urb_setup_isochronous(urb, packets, USB_BT_SCO_OUT_PACKETS);
		if (error == 0)
			error = drv_usb_urb_submit(urb);
		if (error != 0) {
			irq = spin_lock_irqsave(&bt->lock);
			bt->sco_out_busy &= ~(1U << index);
			spin_unlock_irqrestore(&bt->lock, irq);
		}
	}
}

/* Hands one SCO packet put together to the class. */
static void
usb_bt_sco_deliver(
	void *context,
	const uint8_t *packet,
	size_t length)
{
	struct usb_bt *bt;

	/* Queued for the program (the oldest dropped when it does not read). */
	bt = context;
	drv_bt_hci_sco_input(bt->hci, packet, length);
}

/*
 * Waits, asleep a millisecond at a time, until a transfer has retired
 * (design 2.1b B-P1: the SCO thread never yields in a loop to a lower
 * priority's work).  Returns 0, or ETIMEDOUT after USB_BT_DRAIN_MS.
 */
static int
usb_bt_sco_retire(
	struct drv_usb_urb *urb)
{
	unsigned waited;
	int retired;

	/* Until retired or the time is up. */
	for (waited = 0U; waited < USB_BT_DRAIN_MS; waited++) {
		retired = drv_usb_urb_retired(urb);
		if (retired)
			return 0;
		usb_bt_sleep_ms(1U);
	}

	/* Still the host controller's. */
	return ETIMEDOUT;
}
