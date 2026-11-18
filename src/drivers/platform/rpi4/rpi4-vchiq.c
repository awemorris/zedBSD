/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * VCHIQ on the Raspberry Pi 4 (see rpi4-vchiq.h).
 *
 * The ARM is the slave: it allocates the shared memory below the
 * VPU's 1 GiB window, lays out slot zero (rpi4-vchiq-wire.c), posts its bus
 * address on mailbox channel 3 and sends CONNECT.  One receive thread reads
 * the VPU's messages, completes bulk transfers and hands the VPU's slots
 * back; clients send from their own threads under one transmit lock.
 *
 * The doorbell block (the tree's reg) has two registers: BELL0 at +0x00,
 * which the VPU rings for the ARM and which clears when read (bit 2 says it
 * rang), and BELL2 at +0x08, which the ARM writes to ring the VPU.
 */

#include "drivers/platform/rpi4/rpi4-vchiq.h"
#include "drivers/platform/rpi4/rpi4-vchiq-wire.h"
#include "drivers/platform/rpi4/rpi4-board.h"
#include "drivers/platform/rpi4/rpi4-firmware.h"
#include <drivers/generic/dma.h>
#include <drivers/generic/fdt.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/irq.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/pmem.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <kern/waitq.h>
#include <uapi/errno.h>

/* How much of memory at the device tree's address may be read. */
#define VCHIQ_FDT_LIMIT			(1024U * 1024U)

/* The doorbell registers, from the tree's reg. */
#define VCHIQ_BELL0			0x00U
#define VCHIQ_BELL2			0x08U
#define VCHIQ_REGISTER_SPAN		0x0cU

/* The BELL0 bit that says the VPU rang the ARM's doorbell. */
#define VCHIQ_BELL0_RUNG		0x4U

/* The mailbox channel the shared memory's address goes to. */
#define VCHIQ_MAILBOX_CHANNEL		3U

/* The VPU's window onto the first 1 GiB of RAM, uncached. */
#define VCHIQ_BUS_RAM			0xc0000000U
#define VCHIQ_BUS_RAM_LIMIT		0x3fffffffU

/*
 * The fragment area the firmware is told of: it is for page lists whose
 * reads do not start or end on a cache line, which this driver never makes
 * (its memory is uncached), but the firmware reads the area's place.
 */
#define VCHIQ_FRAGMENT_BYTES		128U
#define VCHIQ_FRAGMENT_COUNT		64U

/* The page lists one bulk each, kept with the slots. */
#define VCHIQ_PAGELIST_BYTES		128U
#define VCHIQ_PAGELIST_COUNT		32U

/* The services one boot may have open at once; a service's port is its index plus one. */
#define VCHIQ_SERVICES			8U

/* The bulks of one direction the protocol lets a service have outstanding. */
#define VCHIQ_SERVICE_BULKS		4U

/* How long a caller waits for the connection, an answer, or a slot. */
#define VCHIQ_CONNECT_WAIT_MS		2000U
#define VCHIQ_ANSWER_WAIT_MS		1000U
#define VCHIQ_SLOT_WAIT_MS		1000U

/* The longest sleep for a recycled slot before the ring is looked at again. */
#define VCHIQ_RECYCLE_WAIT_MS		2U

/* The empty control replies (CLOSE, PAUSE, REMOTE_USE_ACTIVE) the receive thread may have waiting. */
#define VCHIQ_CONTROL_PENDING		16U

/* How often the receive thread looks at the queues without a doorbell. */
#define VCHIQ_POLL_MS			20U

/* The open message's payload: the code, the client id and two versions. */
#define VCHIQ_OPEN_BYTES		12U

/* The bulk message's payload: the page list's bus address and the length. */
#define VCHIQ_BULK_BYTES		8U

/* Where a service is in its life. */
enum vchiq_service_state {
	VCHIQ_SERVICE_FREE,
	VCHIQ_SERVICE_OPENING,
	VCHIQ_SERVICE_OPEN,
	VCHIQ_SERVICE_CLOSING,
	VCHIQ_SERVICE_CLOSED
};

/*
 * One bulk transfer the VPU has not yet completed: what to tell the client
 * and which page list to give back.
 */
struct vchiq_bulk {
	void *context;
	unsigned pagelist;
};

/*
 * The bulks of one direction of one service, completed in the order they
 * were queued: inserted counts the queued ones, completed the finished
 * ones; the difference is at most VCHIQ_SERVICE_BULKS.
 */
struct vchiq_bulk_queue {
	struct vchiq_bulk bulks[VCHIQ_SERVICE_BULKS];
	uint32_t inserted;
	uint32_t completed;
};

/*
 * One service a client opened.
 *
 * The state, the ports and the bulk queues change under the driver's lock;
 * calling is set while the receive thread runs one of the client's
 * callbacks, so closing can wait for it to return.
 */
struct drv_rpi4_vchiq_service {
	enum vchiq_service_state state;
	uint32_t fourcc;
	uint32_t local_port;
	uint32_t remote_port;
	uint16_t peer_version;
	const struct drv_rpi4_vchiq_service_ops *ops;
	void *context;
	struct vchiq_bulk_queue transmit;
	struct vchiq_bulk_queue receive;
	unsigned calling;
};

/*
 * The one VCHIQ link of the boot.
 *
 * lock (taken with interrupts off) guards the services, the page list
 * mask, connected, the pending control replies and the three wait queues;
 * send_lock orders writers of the ARM's message stream.  The receive
 * thread never waits for send_lock: its replies wait in control until a
 * writer, or the receive thread when the lock is free, sends them.  The receive thread alone reads the VPU's
 * stream.  The link is never torn down.
 */
struct rpi4_vchiq {
	volatile uint8_t *registers;
	unsigned irq;
	struct drv_dma_device *dma_device;
	struct drv_dma_buffer shared;
	uint8_t *pagelists;
	uint32_t pagelists_bus;
	uint32_t pagelists_free;
	struct drv_rpi4_vchiq_ring ring;
	struct spinlock lock;
	struct mutex send_lock;
	struct wait_queue receive_wake;
	struct wait_queue slot_wake;
	struct wait_queue state_wake;
	struct thread *thread;
	bool connected;
	bool broken;
	struct drv_rpi4_vchiq_service services[VCHIQ_SERVICES];
	uint32_t control[VCHIQ_CONTROL_PENDING];
	unsigned control_head;
	unsigned control_count;
};

/*
 * The link, once drv_rpi4_vchiq_init() has started it.
 *
 * Stored with release after the link is complete and never cleared, so a
 * caller that loads it with acquire sees the whole link; NULL means no
 * VCHIQ (not configured in the tree, an emulator, or a failed start).
 */
static struct rpi4_vchiq *vchiq_link;

static int describe(const struct drv_fdt *fdt, uint64_t *registers, unsigned *irq);
static int start(struct rpi4_vchiq *vchiq, uint64_t registers);
static struct rpi4_vchiq *link_get(void);
static void vchiq_interrupt(int irq, kern_irq_ack_t acknowledge, void *argument);
static void receive_main(void *argument);
static void receive_all(struct rpi4_vchiq *vchiq);
static void dispatch(struct rpi4_vchiq *vchiq, uint32_t msgid, uint32_t size, const uint8_t *payload);
static void dispatch_openack(struct rpi4_vchiq *vchiq, struct drv_rpi4_vchiq_service *service, uint32_t source, uint32_t size, const uint8_t *payload);
static void dispatch_close(struct rpi4_vchiq *vchiq, struct drv_rpi4_vchiq_service *service, uint32_t source);
static void dispatch_data(struct rpi4_vchiq *vchiq, struct drv_rpi4_vchiq_service *service, uint32_t source, uint32_t size, const uint8_t *payload);
static void dispatch_bulk_done(struct rpi4_vchiq *vchiq, struct drv_rpi4_vchiq_service *service, uint32_t source, uint32_t type, uint32_t size, const uint8_t *payload);
static struct drv_rpi4_vchiq_service *service_by_port(struct rpi4_vchiq *vchiq, uint32_t port);
static int send_message(struct rpi4_vchiq *vchiq, uint32_t msgid, const void *data, uint32_t size);
static void queue_control(struct rpi4_vchiq *vchiq, uint32_t msgid);
static void flush_control(struct rpi4_vchiq *vchiq);
static bool bulk_open(const struct drv_rpi4_vchiq_service *service);
static bool bulk_full(const struct vchiq_bulk_queue *queue);
static int reserve_wait(struct rpi4_vchiq *vchiq, uint32_t size, uint8_t **payload);
static void ring_vpu(struct rpi4_vchiq *vchiq, struct drv_rpi4_vchiq_event *event);
static int wait_state(struct rpi4_vchiq *vchiq, struct drv_rpi4_vchiq_service *service, enum vchiq_service_state from, unsigned milliseconds);
static int wait_connected(struct rpi4_vchiq *vchiq);
static int pagelist_take(struct rpi4_vchiq *vchiq, unsigned *index);
static void pagelist_give(struct rpi4_vchiq *vchiq, unsigned index);
static void store_u32(uint8_t *bytes, uint32_t value);
static void store_u16(uint8_t *bytes, uint16_t value);
static uint32_t load_u32(const uint8_t *bytes);
static uint16_t load_u16(const uint8_t *bytes);

/*
 * Finds VCHIQ in the device tree, hands its shared memory to the firmware
 * and starts connecting.
 */
int
drv_rpi4_vchiq_init(
	uint64_t fdt_physical)
{
	struct rpi4_vchiq *vchiq;
	struct drv_fdt fdt;
	const void *blob;
	uint64_t registers;
	unsigned irq;
	bool emulated;
	int error;

	/* Maps the firmware's tree through managed physical RAM and opens it. */
	blob = kern_pmem_to_kernel(fdt_physical);
	if (blob == NULL)
		return ENODEV;
	error = drv_fdt_open(&fdt, blob, VCHIQ_FDT_LIMIT);
	if (error != 0)
		return error;

	/* QEMU's raspi4b has no VPU behind the doorbell. */
	emulated = drv_rpi4_board_emulated(&fdt);
	if (emulated) {
		kern_logf("rpi4-vchiq: emulated board without a VPU; no VCHIQ\n");
		return ENODEV;
	}

	/* Finds the doorbell and its interrupt. */
	error = describe(&fdt, &registers, &irq);
	if (error != 0) {
		kern_logf("rpi4-vchiq: no VCHIQ in the device tree (%d)\n", error);
		return error;
	}

	/* The mailbox carries the shared memory's address. */
	error = drv_rpi4_firmware_init(&fdt);
	if (error != 0) {
		kern_logf("rpi4-vchiq: no firmware mailbox (%d)\n", error);
		return error;
	}

	/* Allocates the link; it lives as long as the kernel. */
	vchiq = kern_calloc(1U, sizeof(*vchiq));
	if (vchiq == NULL)
		return ENOMEM;
	vchiq->irq = irq;

	/* Starts the link; a failure leaves the boot without VCHIQ. */
	error = start(vchiq, registers);
	if (error != 0) {
		kern_logf("rpi4-vchiq: start failed (%d)\n", error);
		return error;
	}

	/* Publishes the link to clients. */
	__atomic_store_n(&vchiq_link, vchiq, __ATOMIC_RELEASE);

	/* Succeeded: the connection completes in the background. */
	kern_logf("rpi4-vchiq: shared memory at bus %08x, irq %u, connecting\n",
		  (unsigned)vchiq->shared.device_address, irq);
	return 0;
}

/*
 * Reports whether the firmware has answered the connection.
 */
bool
drv_rpi4_vchiq_connected(void)
{
	struct rpi4_vchiq *vchiq;
	unsigned long enabled;
	bool connected;

	/* No link means no connection. */
	vchiq = link_get();
	if (vchiq == NULL)
		return false;

	/* Reads the flag the receive thread sets. */
	enabled = spin_lock_irqsave(&vchiq->lock);

	connected = vchiq->connected;
	if (vchiq->broken)
		connected = false;

	spin_unlock_irqrestore(&vchiq->lock, enabled);

	/* Succeeded: reports the connection. */
	return connected;
}

/*
 * Opens a service of the firmware.
 */
int
drv_rpi4_vchiq_open(
	uint32_t fourcc,
	uint16_t version,
	uint16_t version_min,
	const struct drv_rpi4_vchiq_service_ops *ops,
	void *context,
	struct drv_rpi4_vchiq_service **result)
{
	struct rpi4_vchiq *vchiq;
	struct drv_rpi4_vchiq_service *service;
	unsigned long enabled;
	uint8_t payload[VCHIQ_OPEN_BYTES];
	uint32_t msgid;
	unsigned index;
	int error;

	/* The caller receives nothing on failure. */
	*result = NULL;
	vchiq = link_get();
	if (vchiq == NULL)
		return ENODEV;

	/* Waits for the firmware to answer the connection. */
	error = wait_connected(vchiq);
	if (error != 0)
		return error;

	/* Takes a free service entry; its index fixes the local port. */
	enabled = spin_lock_irqsave(&vchiq->lock);

	service = NULL;
	for (index = 0; index < VCHIQ_SERVICES; index++) {
		if (vchiq->services[index].state == VCHIQ_SERVICE_FREE) {
			service = &vchiq->services[index];
			break;
		}
	}

	/* A free entry is set up for the open. */
	if (service != NULL) {
		kern_memset(service, 0, sizeof(*service));
		service->state = VCHIQ_SERVICE_OPENING;
		service->fourcc = fourcc;
		service->local_port = index + 1U;
		service->ops = ops;
		service->context = context;
	}

	spin_unlock_irqrestore(&vchiq->lock, enabled);

	/* Reports a full service table. */
	if (service == NULL)
		return EBUSY;

	/* Asks the firmware for the service by its code and versions. */
	store_u32(&payload[0], fourcc);
	store_u32(&payload[4], 0);
	store_u16(&payload[8], version);
	store_u16(&payload[10], version_min);
	msgid = drv_rpi4_vchiq_msgid(DRV_RPI4_VCHIQ_MSG_OPEN, service->local_port, 0);
	error = send_message(vchiq, msgid, payload, sizeof(payload));
	if (error == 0)
		error = wait_state(vchiq, service, VCHIQ_SERVICE_OPENING, VCHIQ_ANSWER_WAIT_MS);

	/* An unanswered or refused open frees the entry; a late answer finds it free. */
	enabled = spin_lock_irqsave(&vchiq->lock);

	if (error == 0 && service->state != VCHIQ_SERVICE_OPEN)
		error = ECONNREFUSED;
	if (error != 0)
		service->state = VCHIQ_SERVICE_FREE;

	spin_unlock_irqrestore(&vchiq->lock, enabled);

	/* Reports why the service did not open. */
	if (error != 0) {
		kern_logf("rpi4-vchiq: open of %08x failed (%d)\n", (unsigned)fourcc, error);
		return error;
	}

	/* Succeeded: the service carries messages and bulks. */
	*result = service;
	return 0;
}

/*
 * Sends one message on an open service.
 */
int
drv_rpi4_vchiq_send(
	struct drv_rpi4_vchiq_service *service,
	const void *data,
	uint32_t size)
{
	struct rpi4_vchiq *vchiq;
	unsigned long enabled;
	uint32_t msgid;
	bool open;
	int error;

	/* Refuses a service that is not open. */
	vchiq = link_get();
	if (vchiq == NULL)
		return ENODEV;
	enabled = spin_lock_irqsave(&vchiq->lock);

	open = false;
	if (service->state == VCHIQ_SERVICE_OPEN)
		open = true;
	msgid = drv_rpi4_vchiq_msgid(DRV_RPI4_VCHIQ_MSG_DATA, service->local_port, service->remote_port);

	spin_unlock_irqrestore(&vchiq->lock, enabled);

	if (!open)
		return ENOTCONN;

	/* Writes the message into the ARM's stream and rings the VPU. */
	error = send_message(vchiq, msgid, data, size);
	if (error != 0)
		return error;

	/* Succeeded: the VPU has the message to read. */
	return 0;
}

/*
 * Sends an optional message and then queues one bulk transfer.
 *
 * Both go out under one hold of the send lock, so the firmware sees the
 * bulk right after the message that announced it.  The bulk is entered in
 * its queue before its message is published, so its completion always
 * finds it.
 */
int
drv_rpi4_vchiq_send_bulk(
	struct drv_rpi4_vchiq_service *service,
	const void *data,
	uint32_t size,
	unsigned direction,
	uint64_t physical,
	uint32_t bytes,
	void *bulk_context)
{
	struct rpi4_vchiq *vchiq;
	struct vchiq_bulk_queue *queue;
	unsigned long enabled;
	uint8_t *message;
	uint8_t *pagelist;
	size_t used;
	uint32_t data_msgid;
	uint32_t bulk_msgid;
	uint32_t type;
	uint16_t pagelist_type;
	uint64_t deadline;
	uint64_t observed;
	unsigned index;
	bool open;
	bool full;
	int error;

	/* Refuses an unknown direction. */
	vchiq = link_get();
	if (vchiq == NULL)
		return ENODEV;
	if (direction == DRV_RPI4_VCHIQ_BULK_TRANSMIT) {
		type = DRV_RPI4_VCHIQ_MSG_BULK_TX;
		pagelist_type = DRV_RPI4_VCHIQ_PAGELIST_WRITE;
	} else if (direction == DRV_RPI4_VCHIQ_BULK_RECEIVE) {
		type = DRV_RPI4_VCHIQ_MSG_BULK_RX;
		pagelist_type = DRV_RPI4_VCHIQ_PAGELIST_READ;
	} else {
		return EINVAL;
	}

	/* Builds the page list of the buffer in a page list of its own. */
	error = pagelist_take(vchiq, &index);
	if (error != 0)
		return error;
	pagelist = vchiq->pagelists + (size_t)index * VCHIQ_PAGELIST_BYTES;
	error = drv_rpi4_vchiq_pagelist(pagelist, VCHIQ_PAGELIST_BYTES, physical, bytes, pagelist_type, &used);
	if (error != 0) {
		pagelist_give(vchiq, index);
		return error;
	}

	/*
	 * Waits, without the send lock, up to the slot wait limit while the
	 * direction's queue is full (the receive thread completes bulks
	 * without it); then, under the send lock (which keeps the message and
	 * the bulk together), enters the bulk if the queue still has room.
	 */
	deadline = sched_ticks() + kern_ms_to_ticks(VCHIQ_SLOT_WAIT_MS) + 1U;
	queue = &service->transmit;
	if (direction == DRV_RPI4_VCHIQ_BULK_RECEIVE)
		queue = &service->receive;
	for (;;) {
		enabled = spin_lock_irqsave(&vchiq->lock);

		error = 0;
		for (;;) {
			open = bulk_open(service);
			full = bulk_full(queue);
			if (!open || !full)
				break;
			observed = waitq_sequence(&vchiq->state_wake);
			error = waitq_sleep(&vchiq->state_wake, &vchiq->lock, observed, deadline, 0);
			if (error == ETIMEDOUT)
				break;
		}

		spin_unlock_irqrestore(&vchiq->lock, enabled);

		if (!open || full) {
			pagelist_give(vchiq, index);
			if (!open)
				return ENOTCONN;
			return EBUSY;
		}

		/* Takes the send lock, and the room if nobody took it meanwhile. */
		mutex_lock(&vchiq->send_lock);
		enabled = spin_lock_irqsave(&vchiq->lock);

		open = bulk_open(service);
		full = bulk_full(queue);
		if (open && !full) {
			queue->bulks[queue->inserted % VCHIQ_SERVICE_BULKS].context = bulk_context;
			queue->bulks[queue->inserted % VCHIQ_SERVICE_BULKS].pagelist = index;
			queue->inserted++;
		}

		/* The message's identifiers. */
		data_msgid = drv_rpi4_vchiq_msgid(DRV_RPI4_VCHIQ_MSG_DATA, service->local_port, service->remote_port);
		bulk_msgid = drv_rpi4_vchiq_msgid(type, service->local_port, service->remote_port);

		spin_unlock_irqrestore(&vchiq->lock, enabled);

		if (open && !full)
			break;
		mutex_unlock(&vchiq->send_lock);
	}

	/* The announcing message, when there is one. */
	error = 0;
	if (size != 0) {
		error = reserve_wait(vchiq, size, &message);
		if (error == 0) {
			kern_memcpy(message, data, size);
			drv_rpi4_vchiq_ring_commit(&vchiq->ring, data_msgid, size);
		}
	}

	/* The bulk: the page list's bus address and the length. */
	if (error == 0) {
		error = reserve_wait(vchiq, VCHIQ_BULK_BYTES, &message);
		if (error == 0) {
			store_u32(&message[0], vchiq->pagelists_bus + (uint32_t)index * VCHIQ_PAGELIST_BYTES);
			store_u32(&message[4], bytes);
			drv_rpi4_vchiq_ring_commit(&vchiq->ring, bulk_msgid, VCHIQ_BULK_BYTES);
		}
	}

	/*
	 * A bulk whose message never went out is taken back while the send
	 * lock still keeps every other bulk behind it.  The announcing message
	 * may have gone out alone; the client's protocol then stalls and its
	 * own timeouts report it.
	 */
	if (error != 0) {
		enabled = spin_lock_irqsave(&vchiq->lock);

		queue->inserted--;

		spin_unlock_irqrestore(&vchiq->lock, enabled);

		pagelist_give(vchiq, index);
	}

	/* Rings the VPU for whatever was published, with the receive thread's replies. */
	flush_control(vchiq);
	ring_vpu(vchiq, &vchiq->ring.remote->trigger);

	mutex_unlock(&vchiq->send_lock);

	/* Reports a bulk that was not queued. */
	if (error != 0) {
		kern_logf("rpi4-vchiq: bulk of %u bytes not queued (%d)\n", (unsigned)bytes, error);
		return error;
	}

	/* Succeeded: bulk_done reports the transfer. */
	return 0;
}

/*
 * Reports the version the firmware's side of a service speaks.
 */
uint16_t
drv_rpi4_vchiq_peer_version(
	const struct drv_rpi4_vchiq_service *service)
{
	/* Set once by the open's answer. */
	return service->peer_version;
}

/*
 * Closes a service and waits for the firmware to agree.
 */
int
drv_rpi4_vchiq_close(
	struct drv_rpi4_vchiq_service *service)
{
	struct rpi4_vchiq *vchiq;
	unsigned long enabled;
	uint64_t observed;
	uint32_t msgid;
	bool send;
	int error;

	/* A service of no link was never opened. */
	vchiq = link_get();
	if (vchiq == NULL)
		return ENODEV;

	/* An open service is told to close; one the firmware closed is already closed. */
	enabled = spin_lock_irqsave(&vchiq->lock);

	send = false;
	if (service->state == VCHIQ_SERVICE_OPEN) {
		send = true;
		service->state = VCHIQ_SERVICE_CLOSING;
	}

	/* The CLOSE message's identifiers. */
	msgid = drv_rpi4_vchiq_msgid(DRV_RPI4_VCHIQ_MSG_CLOSE, service->local_port, service->remote_port);

	spin_unlock_irqrestore(&vchiq->lock, enabled);

	/* Sends CLOSE and waits for the firmware's CLOSE back. */
	error = 0;
	if (send) {
		error = send_message(vchiq, msgid, NULL, 0);
		if (error == 0)
			error = wait_state(vchiq, service, VCHIQ_SERVICE_CLOSING, VCHIQ_ANSWER_WAIT_MS);
	}

	/* Waits for a callback in progress, then frees the entry. */
	enabled = spin_lock_irqsave(&vchiq->lock);

	while (service->calling != 0) {
		observed = waitq_sequence(&vchiq->state_wake);
		(void)waitq_sleep(&vchiq->state_wake, &vchiq->lock, observed, 0, 0);
	}

	/* Reports bulks the firmware never completed. */
	if (service->receive.inserted != service->receive.completed ||
	    service->transmit.inserted != service->transmit.completed) {
		/* The VPU may still write the buffers; their page lists stay taken. */
		kern_logf("rpi4-vchiq: service %08x closed with bulks outstanding\n", (unsigned)service->fourcc);
	}

	/* The entry is free for the next open. */
	service->state = VCHIQ_SERVICE_FREE;

	spin_unlock_irqrestore(&vchiq->lock, enabled);

	/* Reports a firmware that did not agree in time. */
	if (error != 0)
		return error;

	/* Succeeded: the service is closed on both sides. */
	return 0;
}

/*
 * Logs a service's state, ports and bulk queues.
 */
void
drv_rpi4_vchiq_report(
	struct drv_rpi4_vchiq_service *service)
{
	struct rpi4_vchiq *vchiq;
	unsigned long enabled;
	unsigned state;
	uint32_t local_port;
	uint32_t remote_port;
	uint32_t counts[4];

	/* A service of no link has nothing to report. */
	vchiq = link_get();
	if (vchiq == NULL)
		return;

	/* Copies the fields under the lock and logs them outside it. */
	enabled = spin_lock_irqsave(&vchiq->lock);

	state = (unsigned)service->state;
	local_port = service->local_port;
	remote_port = service->remote_port;
	counts[0] = service->receive.inserted;
	counts[1] = service->receive.completed;
	counts[2] = service->transmit.inserted;
	counts[3] = service->transmit.completed;

	spin_unlock_irqrestore(&vchiq->lock, enabled);

	kern_logf("rpi4-vchiq: service %08x state %u ports %u/%u receive queued %u completed %u transmit queued %u completed %u\n",
		  (unsigned)service->fourcc, state, (unsigned)local_port, (unsigned)remote_port,
		  (unsigned)counts[0], (unsigned)counts[1], (unsigned)counts[2], (unsigned)counts[3]);
}

/*
 * Reads the doorbell's registers and interrupt from the tree.
 */
static int
describe(
	const struct drv_fdt *fdt,
	uint64_t *registers,
	unsigned *irq)
{
	const uint8_t *value;
	uint64_t size;
	uint64_t kind;
	uint32_t length;
	uint32_t node;
	bool enabled;
	int error;

	/* The Pi 4's VCHIQ node, which must be enabled. */
	error = drv_fdt_find_compatible(fdt, "brcm,bcm2711-vchiq", DRV_FDT_NO_NODE, &node);
	if (error != 0)
		return error;
	enabled = drv_fdt_node_enabled(fdt, node);
	if (!enabled)
		return ENODEV;

	/* The doorbell block must hold BELL0 and BELL2. */
	error = drv_fdt_reg(fdt, node, 0, registers, &size);
	if (error != 0)
		return error;
	if (size < VCHIQ_REGISTER_SPAN)
		return ERANGE;

	/* One GIC SPI, three cells. */
	error = drv_fdt_property(fdt, node, "interrupts", &value, &length);
	if (error != 0)
		return error;
	if (length < 12U)
		return EINVAL;
	kind = drv_fdt_cells_value(value, 0, 1);
	if (kind != 0U)
		return ENOTSUP;
	*irq = (unsigned)drv_fdt_cells_value(value, 1, 1) + 32U;
	if (*irq > 1019U)
		return ERANGE;

	/* Succeeded: the doorbell is known. */
	return 0;
}

/*
 * Maps the doorbell, lays out the shared memory, starts the receive thread,
 * hands the memory to the firmware and sends CONNECT.
 *
 * Procedure referenced: FreeBSD sys/contrib/vchiq and the Raspberry Pi
 * userland (the VCHIQ connection and slot protocol).
 */
static int
start(

	struct rpi4_vchiq *vchiq,
	uint64_t registers)
{
	struct drv_dma_constraints constraints;
	void *mapped;
	size_t slot_bytes;
	size_t total;
	uint32_t fragments_bus;
	int error;

	/* The locks and wait queues, before anything can signal them. */
	spin_init(&vchiq->lock, LOCK_RANK_DEVICE, "rpi4 vchiq");
	error = mutex_init(&vchiq->send_lock, LOCK_RANK_DEVICE, "rpi4 vchiq send");
	if (error != 0)
		return error;
	waitq_init(&vchiq->receive_wake, "rpi4 vchiq receive");
	waitq_init(&vchiq->slot_wake, "rpi4 vchiq slot");
	waitq_init(&vchiq->state_wake, "rpi4 vchiq state");

	/* Maps the doorbell registers. */
	error = kern_device_map(registers, VCHIQ_REGISTER_SPAN, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0)
		return error;
	vchiq->registers = mapped;

	/* The slots, the fragment area and the page lists, in the VPU's window. */
	kern_memset(&constraints, 0, sizeof(constraints));
	constraints.address_bits = 32U;
	constraints.max_segment_size = 1024U * 1024U;
	constraints.coherent = 0;
	error = drv_dma_device_create_window(&constraints, VCHIQ_BUS_RAM, VCHIQ_BUS_RAM_LIMIT, &vchiq->dma_device);
	if (error != 0)
		return error;
	slot_bytes = drv_rpi4_vchiq_slot_bytes();
	total = slot_bytes + VCHIQ_FRAGMENT_BYTES * VCHIQ_FRAGMENT_COUNT + VCHIQ_PAGELIST_BYTES * VCHIQ_PAGELIST_COUNT;
	error = drv_dma_alloc_coherent(vchiq->dma_device, total, DRV_RPI4_VCHIQ_SLOT_SIZE, &vchiq->shared);
	if (error != 0)
		return error;
	kern_memset(vchiq->shared.address, 0, total);

	/* Lays out slot zero with the fragment area right after the slots. */
	fragments_bus = (uint32_t)vchiq->shared.device_address + (uint32_t)slot_bytes;
	error = drv_rpi4_vchiq_ring_init(&vchiq->ring, vchiq->shared.address, slot_bytes, fragments_bus, VCHIQ_FRAGMENT_COUNT);
	if (error != 0)
		return error;

	/* The page lists follow the fragment area; all are free. */
	vchiq->pagelists = (uint8_t *)vchiq->shared.address + slot_bytes + VCHIQ_FRAGMENT_BYTES * VCHIQ_FRAGMENT_COUNT;
	vchiq->pagelists_bus = fragments_bus + VCHIQ_FRAGMENT_BYTES * VCHIQ_FRAGMENT_COUNT;
	vchiq->pagelists_free = 0xffffffffU;

	/* The doorbell's interrupt, level high. */
	kern_irq_mask((int)vchiq->irq);
	error = kern_irq_set_mode((int)vchiq->irq, KERN_IRQ_TRIGGER_LEVEL, KERN_IRQ_POLARITY_HIGH);
	if (error != 0)
		return error;
	error = kern_irq_register((int)vchiq->irq, vchiq_interrupt, vchiq);
	if (error != 0)
		return error;

	/* The receive thread, which also notices the VPU without the doorbell. */
	error = kthread_create(receive_main, vchiq, SCHED_PRIORITY_DEFAULT, &vchiq->thread);
	if (error != 0) {
		(void)kern_irq_unregister((int)vchiq->irq, vchiq_interrupt, vchiq);
		return error;
	}

	/* Starts the receive thread and lets the doorbell interrupt in. */
	thread_start(vchiq->thread);
	kern_irq_unmask((int)vchiq->irq);

	/* Hands the firmware slot zero's bus address. */
	error = drv_rpi4_firmware_mailbox_post(VCHIQ_MAILBOX_CHANNEL, (uint32_t)vchiq->shared.device_address);
	if (error != 0)
		return error;

	/* Asks the firmware to connect; its CONNECT comes back to the receive thread. */
	error = send_message(vchiq, drv_rpi4_vchiq_msgid(DRV_RPI4_VCHIQ_MSG_CONNECT, 0, 0), NULL, 0);
	if (error != 0)
		return error;

	/* Succeeded: the link is running. */
	return 0;
}

/* Loads the published link, or NULL. */
static struct rpi4_vchiq *
link_get(void)
{
	/* The acquire load orders the reads of the link after its publication. */
	return __atomic_load_n(&vchiq_link, __ATOMIC_ACQUIRE);
}

/* Wakes the receive thread when the VPU rang the ARM's doorbell. */
static void
vchiq_interrupt(
	int irq,
	kern_irq_ack_t acknowledge,
	void *argument)
{
	struct rpi4_vchiq *vchiq;
	unsigned long enabled;
	uint32_t status;

	/* Reading BELL0 clears it. */
	(void)irq;
	vchiq = argument;
	status = kern_mmio_read32(vchiq->registers + VCHIQ_BELL0);

	/* A rung doorbell means one of the ARM's events fired. */
	if ((status & VCHIQ_BELL0_RUNG) != 0) {
		enabled = spin_lock_irqsave(&vchiq->lock);

		waitq_wake_all(&vchiq->receive_wake);

		spin_unlock_irqrestore(&vchiq->lock, enabled);
	}

	/* Completes the level acknowledgement. */
	kern_irq_send_eoi(acknowledge);
}

/*
 * Reads the VPU's messages and passes its recycled slots to writers, for
 * the kernel's lifetime.
 *
 * The ARM's events are left armed, so the VPU rings the doorbell whenever
 * it fires one; the thread also looks every VCHIQ_POLL_MS in case a ring
 * was missed.
 */
static void
receive_main(
	void *argument)
{
	struct rpi4_vchiq *vchiq;
	struct drv_rpi4_vchiq_shared *local;
	unsigned long enabled;
	uint64_t observed;
	uint64_t deadline;
	bool sync_logged;
	int locked;

	/* The link lives for the kernel's lifetime. */
	vchiq = argument;
	local = vchiq->ring.local;
	sync_logged = false;

	/* Serves the link until the kernel stops. */
	for (;;) {
		/* Arms the events, so the VPU rings for each one it fires. */
		local->trigger.armed = 1U;
		local->recycle.armed = 1U;
		local->sync_trigger.armed = 1U;
		kern_io_barrier();

		/* Reads every message the VPU has published. */
		local->trigger.fired = 0;
		kern_io_barrier();
		receive_all(vchiq);

		/* Sends the replies the messages asked for, when no writer holds the stream. */
		locked = mutex_trylock(&vchiq->send_lock);
		if (locked) {
			flush_control(vchiq);
			if (!vchiq->connected)
				ring_vpu(vchiq, &vchiq->ring.remote->trigger);
			mutex_unlock(&vchiq->send_lock);
		}

		/* Recycled slots: writers waiting for room look again. */
		if (local->recycle.fired != 0) {
			local->recycle.fired = 0;
			enabled = spin_lock_irqsave(&vchiq->lock);

			waitq_wake_all(&vchiq->slot_wake);

			spin_unlock_irqrestore(&vchiq->lock, enabled);
		}

		/* The ARM opens no synchronous service, so a synchronous message is unexpected. */
		if (local->sync_trigger.fired != 0) {
			local->sync_trigger.fired = 0;
			if (!sync_logged) {
				sync_logged = true;
				kern_logf("rpi4-vchiq: unexpected synchronous message ignored\n");
			}
		}

		/* Sleeps until the doorbell rings or the poll interval passes. */
		enabled = spin_lock_irqsave(&vchiq->lock);

		observed = waitq_sequence(&vchiq->receive_wake);
		if (local->trigger.fired == 0 && local->recycle.fired == 0) {
			deadline = sched_ticks() + kern_ms_to_ticks(VCHIQ_POLL_MS) + 1U;
			(void)waitq_sleep(&vchiq->receive_wake, &vchiq->lock, observed, deadline, 0);
		}

		spin_unlock_irqrestore(&vchiq->lock, enabled);
	}
}

/*
 * Reads every message the VPU has published and hands each slot back once
 * it has been read to its end.
 */
static void
receive_all(
	struct rpi4_vchiq *vchiq)
{
	const uint8_t *payload;
	uint32_t msgid;
	uint32_t size;
	bool released;
	int error;

	/* A malformed stream stops the link for good. */
	if (vchiq->broken)
		return;

	/* Reads messages until the ARM has caught up with the VPU. */
	for (;;) {
		error = drv_rpi4_vchiq_ring_next(&vchiq->ring, &msgid, &size, &payload);
		if (error == EAGAIN)
			break;
		if (error != 0) {
			kern_logf("rpi4-vchiq: malformed message stream at %x; link stopped\n",
				  (unsigned)vchiq->ring.rx_pos);
			vchiq->broken = true;
			break;
		}

		/* Acts on the message, then moves past it. */
		dispatch(vchiq, msgid, size, payload);
		released = drv_rpi4_vchiq_ring_advance(&vchiq->ring, size);
		if (released)
			ring_vpu(vchiq, &vchiq->ring.remote->recycle);
	}
}

/* Acts on one message of the VPU's stream. */
static void
dispatch(
	struct rpi4_vchiq *vchiq,
	uint32_t msgid,
	uint32_t size,
	const uint8_t *payload)
{
	struct drv_rpi4_vchiq_service *service;
	unsigned long enabled;
	uint32_t type;
	uint32_t source;
	uint32_t destination;

	/* The message's type and ports, and the service its destination port names. */
	type = drv_rpi4_vchiq_msgid_type(msgid);
	source = drv_rpi4_vchiq_msgid_source(msgid);
	destination = drv_rpi4_vchiq_msgid_destination(msgid);
	service = service_by_port(vchiq, destination);

	/* Each type has its own handling. */
	switch (type) {
	case DRV_RPI4_VCHIQ_MSG_PADDING:
		/* Fills the end of a slot; nothing to do. */
		break;
	case DRV_RPI4_VCHIQ_MSG_CONNECT:
		/* The firmware has connected: clients may open services. */
		enabled = spin_lock_irqsave(&vchiq->lock);

		vchiq->connected = true;
		waitq_wake_all(&vchiq->state_wake);

		spin_unlock_irqrestore(&vchiq->lock, enabled);

		kern_logf("rpi4-vchiq: connected (version %u)\n", (unsigned)vchiq->ring.zero->version);
		break;
	case DRV_RPI4_VCHIQ_MSG_OPEN:
		/* The ARM offers no services: refuses the firmware's open. */
		queue_control(vchiq, drv_rpi4_vchiq_msgid(DRV_RPI4_VCHIQ_MSG_CLOSE, 0, source));
		break;
	case DRV_RPI4_VCHIQ_MSG_OPENACK:
		dispatch_openack(vchiq, service, source, size, payload);
		break;
	case DRV_RPI4_VCHIQ_MSG_CLOSE:
		dispatch_close(vchiq, service, source);
		break;
	case DRV_RPI4_VCHIQ_MSG_DATA:
		dispatch_data(vchiq, service, source, size, payload);
		break;
	case DRV_RPI4_VCHIQ_MSG_BULK_RX_DONE:
	case DRV_RPI4_VCHIQ_MSG_BULK_TX_DONE:
		dispatch_bulk_done(vchiq, service, source, type, size, payload);
		break;
	case DRV_RPI4_VCHIQ_MSG_PAUSE:
		/* The ARM never suspends the link: answers a pause at once. */
		queue_control(vchiq, drv_rpi4_vchiq_msgid(DRV_RPI4_VCHIQ_MSG_PAUSE, 0, 0));
		break;
	case DRV_RPI4_VCHIQ_MSG_REMOTE_USE:
		/* The firmware asks the ARM to stay awake: says that it is. */
		queue_control(vchiq, drv_rpi4_vchiq_msgid(DRV_RPI4_VCHIQ_MSG_REMOTE_USE_ACTIVE, 0, 0));
		break;
	case DRV_RPI4_VCHIQ_MSG_RESUME:
	case DRV_RPI4_VCHIQ_MSG_REMOTE_RELEASE:
	case DRV_RPI4_VCHIQ_MSG_REMOTE_USE_ACTIVE:
		/* Power bookkeeping the ARM does not keep. */
		break;
	default:
		/* Bulk requests are the master's to receive; anything else is unknown. */
		kern_logf("rpi4-vchiq: unexpected message %08x (%u bytes)\n", (unsigned)msgid, (unsigned)size);
		break;
	}
}

/* Completes an open the firmware accepted. */
static void
dispatch_openack(
	struct rpi4_vchiq *vchiq,
	struct drv_rpi4_vchiq_service *service,
	uint32_t source,
	uint32_t size,
	const uint8_t *payload)
{
	unsigned long enabled;
	bool late;

	/* An answer for an open that already gave up is closed again. */
	late = true;
	enabled = spin_lock_irqsave(&vchiq->lock);

	if (service != NULL && service->state == VCHIQ_SERVICE_OPENING) {
		late = false;
		service->remote_port = source;
		service->peer_version = 0;
		if (size >= 2U)
			service->peer_version = load_u16(payload);
		service->state = VCHIQ_SERVICE_OPEN;
		waitq_wake_all(&vchiq->state_wake);
	}

	spin_unlock_irqrestore(&vchiq->lock, enabled);

	/* Closes the firmware's side of an open nobody waits for. */
	if (late)
		queue_control(vchiq, drv_rpi4_vchiq_msgid(DRV_RPI4_VCHIQ_MSG_CLOSE, 0, source));
}

/*
 * Handles the firmware's CLOSE: the answer to the ARM's own close, the
 * refusal of an open, or the firmware closing an open service.
 */
static void
dispatch_close(
	struct rpi4_vchiq *vchiq,
	struct drv_rpi4_vchiq_service *service,
	uint32_t source)
{
	unsigned long enabled;
	bool answer;
	bool notify;

	/* Moves the service to closed, deciding what else the close needs. */
	answer = false;
	notify = false;
	enabled = spin_lock_irqsave(&vchiq->lock);

	if (service != NULL &&
	    service->state != VCHIQ_SERVICE_OPENING &&
	    service->state != VCHIQ_SERVICE_FREE &&
	    service->remote_port != source) {
		/* A CLOSE from an earlier use of the port is not this service's. */
		service = NULL;
	}

	/* An open service closed by the firmware is answered and its client told. */
	if (service != NULL) {
		if (service->state == VCHIQ_SERVICE_OPEN) {
			answer = true;
			notify = true;
			service->calling++;
		}

		/* Any service that is not free is closed now. */
		if (service->state != VCHIQ_SERVICE_FREE)
			service->state = VCHIQ_SERVICE_CLOSED;
		waitq_wake_all(&vchiq->state_wake);
	}

	spin_unlock_irqrestore(&vchiq->lock, enabled);

	/* The firmware closed an open service: agrees, and tells the client. */
	if (answer)
		queue_control(vchiq, drv_rpi4_vchiq_msgid(DRV_RPI4_VCHIQ_MSG_CLOSE, service->local_port, source));
	if (notify) {
		if (service->ops->closed != NULL)
			service->ops->closed(service->context);
		enabled = spin_lock_irqsave(&vchiq->lock);

		service->calling--;
		waitq_wake_all(&vchiq->state_wake);

		spin_unlock_irqrestore(&vchiq->lock, enabled);
	}
}

/* Hands one data message to its service's client. */
static void
dispatch_data(
	struct rpi4_vchiq *vchiq,
	struct drv_rpi4_vchiq_service *service,
	uint32_t source,
	uint32_t size,
	const uint8_t *payload)
{
	unsigned long enabled;
	bool deliver;

	/* Only an open service's own peer is heard; the client is marked busy. */
	deliver = false;
	enabled = spin_lock_irqsave(&vchiq->lock);

	if (service != NULL &&
	    service->state == VCHIQ_SERVICE_OPEN &&
	    service->remote_port == source) {
		deliver = true;
		service->calling++;
	}

	spin_unlock_irqrestore(&vchiq->lock, enabled);

	/* Drops a message for a closed or unknown service. */
	if (!deliver)
		return;

	/* The client copies what it needs while the slot still holds it. */
	service->ops->message(service->context, payload, size);

	/* The client is no longer called; a closer may go on. */
	enabled = spin_lock_irqsave(&vchiq->lock);

	service->calling--;
	waitq_wake_all(&vchiq->state_wake);

	spin_unlock_irqrestore(&vchiq->lock, enabled);
}

/*
 * Completes the oldest outstanding bulk of one direction: the firmware
 * completes a service's bulks in the order they were queued.
 */
static void
dispatch_bulk_done(
	struct rpi4_vchiq *vchiq,
	struct drv_rpi4_vchiq_service *service,
	uint32_t source,
	uint32_t type,
	uint32_t size,
	const uint8_t *payload)
{
	struct vchiq_bulk_queue *queue;
	unsigned long enabled;
	void *context;
	unsigned pagelist;
	int32_t actual;
	bool found;

	/* The actual length the firmware moved, or -1 for an aborted bulk. */
	actual = DRV_RPI4_VCHIQ_BULK_ABORTED;
	if (size >= 4U)
		actual = (int32_t)load_u32(payload);

	/* Takes the oldest bulk of the direction out of its queue. */
	found = false;
	context = NULL;
	pagelist = 0;
	enabled = spin_lock_irqsave(&vchiq->lock);

	if (service != NULL && service->state != VCHIQ_SERVICE_FREE && service->remote_port == source) {
		queue = &service->transmit;
		if (type == DRV_RPI4_VCHIQ_MSG_BULK_RX_DONE)
			queue = &service->receive;
		if (queue->completed != queue->inserted) {
			found = true;
			context = queue->bulks[queue->completed % VCHIQ_SERVICE_BULKS].context;
			pagelist = queue->bulks[queue->completed % VCHIQ_SERVICE_BULKS].pagelist;
			queue->completed++;
			service->calling++;
		}
	}

	spin_unlock_irqrestore(&vchiq->lock, enabled);

	/* Reports a completion nobody queued. */
	if (!found) {
		kern_logf("rpi4-vchiq: unexpected bulk completion (%u)\n", (unsigned)type);
		return;
	}

	/* Gives the page list back and tells the client. */
	pagelist_give(vchiq, pagelist);
	if (service->ops->bulk_done != NULL)
		service->ops->bulk_done(service->context, context, actual);

	/* The client is no longer called; a closer or a waiting sender may go on. */
	enabled = spin_lock_irqsave(&vchiq->lock);

	service->calling--;
	waitq_wake_all(&vchiq->state_wake);

	spin_unlock_irqrestore(&vchiq->lock, enabled);
}

/* Finds the service a local port names, or NULL. */
static struct drv_rpi4_vchiq_service *
service_by_port(
	struct rpi4_vchiq *vchiq,
	uint32_t port)
{
	/* Ports count services from one. */
	if (port == 0 || port > VCHIQ_SERVICES)
		return NULL;

	/* Succeeded: the entry of the port. */
	return &vchiq->services[port - 1U];
}

/*
 * Writes one message into the ARM's stream and rings the VPU.
 */
static int
send_message(
	struct rpi4_vchiq *vchiq,
	uint32_t msgid,
	const void *data,
	uint32_t size)
{
	uint8_t *payload;
	int error;

	/* One writer at a time reserves, fills and publishes. */
	mutex_lock(&vchiq->send_lock);

	error = reserve_wait(vchiq, size, &payload);
	if (error == 0) {
		if (size != 0)
			kern_memcpy(payload, data, size);
		drv_rpi4_vchiq_ring_commit(&vchiq->ring, msgid, size);
	}

	/* Sends what the receive thread had waiting and rings the VPU. */
	flush_control(vchiq);
	ring_vpu(vchiq, &vchiq->ring.remote->trigger);

	mutex_unlock(&vchiq->send_lock);

	/* Reports a stream that stayed full. */
	if (error != 0)
		return error;

	/* Succeeded: the message is the VPU's to read. */
	return 0;
}

/* Leaves an empty control reply for the next holder of the send lock to send. */
static void
queue_control(
	struct rpi4_vchiq *vchiq,
	uint32_t msgid)
{
	unsigned long enabled;
	bool dropped;

	/* Appends the reply; a full list drops it (the firmware's own timeouts then apply). */
	dropped = false;
	enabled = spin_lock_irqsave(&vchiq->lock);

	if (vchiq->control_count < VCHIQ_CONTROL_PENDING) {
		vchiq->control[(vchiq->control_head + vchiq->control_count) % VCHIQ_CONTROL_PENDING] = msgid;
		vchiq->control_count++;
	} else {
		dropped = true;
	}

	spin_unlock_irqrestore(&vchiq->lock, enabled);

	/* Reports a lost reply. */
	if (dropped)
		kern_logf("rpi4-vchiq: control reply %08x dropped\n", (unsigned)msgid);
}

/*
 * Sends the waiting control replies without waiting for a slot; the
 * caller holds the send lock and rings the VPU afterwards.
 */
static void
flush_control(
	struct rpi4_vchiq *vchiq)
{
	unsigned long enabled;
	uint8_t *payload;
	uint32_t msgid;
	bool empty;
	int error;

	/* One reply at a time, until none waits or no slot is free. */
	for (;;) {
		enabled = spin_lock_irqsave(&vchiq->lock);

		empty = true;
		if (vchiq->control_count != 0)
			empty = false;
		msgid = vchiq->control[vchiq->control_head];

		spin_unlock_irqrestore(&vchiq->lock, enabled);

		if (empty)
			return;
		error = drv_rpi4_vchiq_ring_reserve(&vchiq->ring, 0, &payload);
		if (error == EAGAIN) {
			drv_rpi4_vchiq_ring_recycle(&vchiq->ring);
			error = drv_rpi4_vchiq_ring_reserve(&vchiq->ring, 0, &payload);
		}

		/* Gives up when no room came. */
		if (error != 0)
			return;
		drv_rpi4_vchiq_ring_commit(&vchiq->ring, msgid, 0);
		enabled = spin_lock_irqsave(&vchiq->lock);

		vchiq->control_head = (vchiq->control_head + 1U) % VCHIQ_CONTROL_PENDING;
		vchiq->control_count--;

		spin_unlock_irqrestore(&vchiq->lock, enabled);
	}
}

/*
 * Reserves room for a message, waiting for the VPU to recycle a slot.
 *
 * The caller holds the send lock.  The wait looks at the VPU's recycle
 * count itself at least every two ticks, so it does not depend on the
 * receive thread (which may itself be waiting for the send lock).
 */
static int
reserve_wait(
	struct rpi4_vchiq *vchiq,
	uint32_t size,
	uint8_t **payload)
{
	unsigned long enabled;
	uint64_t observed;
	uint64_t deadline;
	uint64_t wake;
	uint64_t now;
	int error;

	/* Gives up after the slot wait limit. */
	deadline = sched_ticks() + kern_ms_to_ticks(VCHIQ_SLOT_WAIT_MS) + 1U;
	for (;;) {
		/* A link with a broken stream sends nothing more. */
		if (vchiq->broken)
			return EIO;

		/* Tries the current position, then again after taking back recycled slots. */
		error = drv_rpi4_vchiq_ring_reserve(&vchiq->ring, size, payload);
		if (error != EAGAIN)
			return error;
		drv_rpi4_vchiq_ring_recycle(&vchiq->ring);
		error = drv_rpi4_vchiq_ring_reserve(&vchiq->ring, size, payload);
		if (error != EAGAIN)
			return error;

		/* Reports a VPU that kept every slot for too long. */
		now = sched_ticks();
		if (now >= deadline) {
			kern_logf("rpi4-vchiq: no free slot in %u ms\n", VCHIQ_SLOT_WAIT_MS);
			return EIO;
		}

		/* Sleeps until the VPU recycles a slot, or two milliseconds pass. */
		vchiq->ring.local->recycle.armed = 1U;
		kern_io_barrier();
		enabled = spin_lock_irqsave(&vchiq->lock);

		observed = waitq_sequence(&vchiq->slot_wake);
		wake = sched_ticks() + kern_ms_to_ticks(VCHIQ_RECYCLE_WAIT_MS);
		(void)waitq_sleep(&vchiq->slot_wake, &vchiq->lock, observed, wake, 0);

		spin_unlock_irqrestore(&vchiq->lock, enabled);
	}
}

/* Fires one of the VPU's events and rings its doorbell when it sleeps. */
static void
ring_vpu(
	struct rpi4_vchiq *vchiq,
	struct drv_rpi4_vchiq_event *event)
{
	bool wake;

	/* Marks the event fired; only an armed one needs the doorbell. */
	wake = drv_rpi4_vchiq_event_fire(event);
	if (wake)
		kern_mmio_write32(vchiq->registers + VCHIQ_BELL2, 0);
}

/*
 * Waits until a service leaves a state, or the time runs out.
 */
static int
wait_state(
	struct rpi4_vchiq *vchiq,
	struct drv_rpi4_vchiq_service *service,
	enum vchiq_service_state from,
	unsigned milliseconds)
{
	unsigned long enabled;
	uint64_t observed;
	uint64_t deadline;
	int error;

	/* Sleeps on state changes until the service moves or the deadline passes. */
	deadline = sched_ticks() + kern_ms_to_ticks(milliseconds) + 1U;
	error = 0;
	enabled = spin_lock_irqsave(&vchiq->lock);

	while (service->state == from) {
		observed = waitq_sequence(&vchiq->state_wake);
		error = waitq_sleep(&vchiq->state_wake, &vchiq->lock, observed, deadline, 0);
		if (error == ETIMEDOUT && service->state == from)
			break;
		error = 0;
	}

	spin_unlock_irqrestore(&vchiq->lock, enabled);

	/* Reports a firmware that did not answer in time. */
	if (error != 0)
		return error;

	/* Succeeded: the service has moved on. */
	return 0;
}

/* Waits until the firmware has connected, or the time runs out. */
static int
wait_connected(
	struct rpi4_vchiq *vchiq)
{
	unsigned long enabled;
	uint64_t observed;
	uint64_t deadline;
	int error;

	/* Sleeps on state changes until the connection or the deadline. */
	deadline = sched_ticks() + kern_ms_to_ticks(VCHIQ_CONNECT_WAIT_MS) + 1U;
	error = 0;
	enabled = spin_lock_irqsave(&vchiq->lock);

	while (!vchiq->connected) {
		observed = waitq_sequence(&vchiq->state_wake);
		error = waitq_sleep(&vchiq->state_wake, &vchiq->lock, observed, deadline, 0);
		if (error == ETIMEDOUT && !vchiq->connected)
			break;
		error = 0;
	}

	/* A link that broke meanwhile is reported. */
	if (error == 0 && vchiq->broken)
		error = EIO;

	spin_unlock_irqrestore(&vchiq->lock, enabled);

	/* Reports a firmware that never connected. */
	if (error != 0)
		return error;

	/* Succeeded: services may be opened. */
	return 0;
}

/* Takes a free page list. */
static int
pagelist_take(
	struct rpi4_vchiq *vchiq,
	unsigned *index)
{
	unsigned long enabled;
	unsigned candidate;
	int error;

	/* The lowest free page list, if any. */
	error = EBUSY;
	enabled = spin_lock_irqsave(&vchiq->lock);

	for (candidate = 0; candidate < VCHIQ_PAGELIST_COUNT; candidate++) {
		if ((vchiq->pagelists_free & (1U << candidate)) != 0) {
			vchiq->pagelists_free &= ~(1U << candidate);
			*index = candidate;
			error = 0;
			break;
		}
	}

	spin_unlock_irqrestore(&vchiq->lock, enabled);

	/* Reports that every page list is in use. */
	if (error != 0)
		return error;

	/* Succeeded: the page list is the caller's. */
	return 0;
}

/* Gives a page list back. */
static void
pagelist_give(
	struct rpi4_vchiq *vchiq,
	unsigned index)
{
	unsigned long enabled;

	/* Marks the page list free again. */
	enabled = spin_lock_irqsave(&vchiq->lock);

	vchiq->pagelists_free |= 1U << index;

	spin_unlock_irqrestore(&vchiq->lock, enabled);
}

/* Stores a little-endian 32-bit word. */
static void
store_u32(
	uint8_t *bytes,
	uint32_t value)
{
	/* Lowest byte first. */
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
}

/* Stores a little-endian 16-bit word. */
static void
store_u16(
	uint8_t *bytes,
	uint16_t value)
{
	/* Lowest byte first. */
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

/* Loads a little-endian 32-bit word. */
static uint32_t
load_u32(
	const uint8_t *bytes)
{
	uint32_t value;

	/* Lowest byte first. */
	value = bytes[0];
	value |= (uint32_t)bytes[1] << 8;
	value |= (uint32_t)bytes[2] << 16;
	value |= (uint32_t)bytes[3] << 24;

	/* Succeeded: the word. */
	return value;
}

/* Loads a little-endian 16-bit word. */
static uint16_t
load_u16(
	const uint8_t *bytes)
{
	uint16_t value;

	/* Lowest byte first. */
	value = (uint16_t)bytes[0];
	value |= (uint16_t)((uint16_t)bytes[1] << 8);

	/* Succeeded: the word. */
	return value;
}

/* Reports whether a service is open; the caller holds the lock. */
static bool
bulk_open(
	const struct drv_rpi4_vchiq_service *service)
{
	/* Only an open service takes bulks. */
	if (service->state != VCHIQ_SERVICE_OPEN)
		return false;

	/* Succeeded: the service is open. */
	return true;
}

/* Reports whether a bulk queue holds all the bulks the protocol allows; the caller holds the lock. */
static bool
bulk_full(
	const struct vchiq_bulk_queue *queue)
{
	/* The outstanding count is the difference of the two counters. */
	if (queue->inserted - queue->completed >= VCHIQ_SERVICE_BULKS)
		return true;

	/* Succeeded: there is room. */
	return false;
}
