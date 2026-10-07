/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Bluetooth controllers' HCI class (drivers/generic/bt-hci.h,
 * ws143-p002; the node's interface is <uapi/bluetooth.h>).
 *
 * Each registered controller gets the lowest free number N and the
 * character device btN (devfs: /dev/btN, root's alone, mode 0600).  One
 * open at a time holds it.  The transport hands the packets it receives
 * to drv_bt_hci_input(), which queues them, in the events' or the ACL
 * ring, under the controller's spinlock with a sequence number, and wakes
 * the reader; a read gives the packet with the lowest sequence number of
 * the two rings, so the program sees them in the order they came.
 *
 * A write and the requests that act on the controller call the transport
 * under the operations' mutex, one at a time.  drv_bt_hci_withdraw()
 * takes the transport away under the same mutex, so no operation runs
 * after it returns, and waits out a room call in flight; the transport
 * then stops its transfers and calls drv_bt_hci_release(), which
 * unpublishes the node.  The record goes with the node's last reference.
 */

#include <drivers/generic/bt-hci.h>

#include "kern/cdev.h"
#include "kern/file.h"
#include "kern/klog.h"
#include "kern/kmem.h"
#include "kern/lock.h"
#include "kern/poll.h"
#include "kern/sched.h"
#include "kern/uaccess.h"
#include "kern/waitq.h"
#include <kern/kcrt.h>
#include <uapi/errno.h>
#include <uapi/fcntl.h>
#include <uapi/poll.h>

/*
 * The device numbers of the class: one major, the minor is N.  devfs keeps
 * a node's owner and mode by its number, so the major is one no other node
 * has (0x0011 is /dev/typec's; 0x0012 is free of every cdev_register).
 */
#define BT_HCI_DEVICE_BASE	0x00120000U

/* The rings' sizes: the events' (with a share for the notices) and the ACL packets'. */
#define BT_HCI_EVENT_RING	(16U * 1024U)
#define BT_HCI_EVENT_RESERVE	(4U * BT_HCI_RECORD_HEADER)
#define BT_HCI_ACL_RING		(32U * 1024U)

/* One published controller. */
struct drv_bt_hci {
	/*
	 * Guards the rings, the sequence, the counts, the open, registered,
	 * the room calls in flight, the flags and the ACL limit.  The reader
	 * sleeps on waitq under it.
	 */
	struct spinlock lock;
	struct wait_queue waitq;
	struct bt_hci_ring events;
	struct bt_hci_ring acl;
	uint8_t *event_bytes;
	uint8_t *acl_bytes;

	/*
	 * The number the next queued record gets.  It wraps; two records are
	 * compared by the sign of their difference, which holds while the
	 * rings hold far fewer than 2^31 records.
	 */
	uint32_t sequence;

	/* registered is cleared by the withdrawal; opened while one open holds the node. */
	unsigned registered;
	unsigned opened;

	/*
	 * The room calls running now.  The withdrawal waits for it to reach
	 * zero, so the transport's record outlives every call into it.
	 */
	unsigned room_calls;

	/* What the requests give out; the flags and the ACL limit change through the requests. */
	struct bt_info info;
	struct bt_stats stats;

	/*
	 * Serializes the operations with each other and with the withdrawal,
	 * which clears ops: an operation that finds ops NULL answers ENODEV.
	 */
	struct mutex op_lock;
	const struct drv_bt_hci_ops *ops;
	void *context;

	/* The number the controller holds and the node's reference the registration owns. */
	unsigned number;
	struct cdev *node;
};

static int bt_hci_open(struct file *file);
static int bt_hci_close(struct file *file);
static ssize_t bt_hci_read(struct file *file, void *buffer, size_t size);
static ssize_t bt_hci_write(struct file *file, const void *buffer, size_t size);
static int bt_hci_ioctl(struct file *file, unsigned long request, uintptr_t argument);
static int bt_hci_poll(struct file *file, short requested, short *returned);
static struct drv_bt_hci *bt_hci_file_device(struct file *file);
static struct bt_hci_ring *bt_hci_oldest(struct drv_bt_hci *hci);
static void bt_hci_room_call(struct drv_bt_hci *hci);
static int bt_hci_operate(struct drv_bt_hci *hci, unsigned long request, uint32_t value);
static int bt_hci_slot_claim(struct drv_bt_hci *hci);
static void bt_hci_slot_release(struct drv_bt_hci *hci);
static void bt_hci_finalize(void *data);
static void bt_hci_copy_text(char *target, const char *source);

/* The operations of every controller's node. */
static const struct cdev_ops bt_hci_cdev_ops = {
	.open = bt_hci_open,
	.close = bt_hci_close,
	.read = bt_hci_read,
	.write = bt_hci_write,
	.ioctl = bt_hci_ioctl,
	.poll = bt_hci_poll
};

/* The controller holding each number, NULL for a free one; guarded by bt_hci_registry_lock. */
static struct drv_bt_hci *bt_hci_slots[DRV_BT_HCI_MAX];

/* Serializes the claiming and the release of the numbers for the kernel's life. */
static struct spinlock bt_hci_registry_lock = {
	{ 0 }, LOCK_RANK_DEVICE, "bt-hci registry", 0, 0
};

/*
 * Registers a controller and publishes its node.
 *
 * Returns 0 with the controller's record in *result, EINVAL for a missing
 * part, ENOSPC when every number is taken, ENOMEM, or the error of
 * publishing the node.
 */
int
drv_bt_hci_register(
	const struct drv_bt_hci_description *description,
	const struct drv_bt_hci_ops *ops,
	void *context,
	struct drv_bt_hci **result)
{
	struct drv_bt_hci *hci;
	struct cdev *node;
	char node_name[16];
	int error;

	/* A transport describes its controller and does every operation. */
	if (description == NULL || ops == NULL || result == NULL)
		return EINVAL;
	if (ops->send == NULL || ops->set_bootloader == NULL)
		return EINVAL;
	if (ops->reset == NULL || ops->room == NULL)
		return EINVAL;

	/* The controller's record. */
	hci = kern_calloc(1U, sizeof(*hci));
	if (hci == NULL)
		return ENOMEM;

	/* The events' ring. */
	hci->event_bytes = kern_malloc(BT_HCI_EVENT_RING);
	if (hci->event_bytes == NULL) {
		kern_free(hci);
		return ENOMEM;
	}

	/* The ACL packets' ring. */
	hci->acl_bytes = kern_malloc(BT_HCI_ACL_RING);
	if (hci->acl_bytes == NULL) {
		kern_free(hci->event_bytes);
		kern_free(hci);
		return ENOMEM;
	}

	/* The operations' lock. */
	error = mutex_init(&hci->op_lock, LOCK_RANK_DEVICE, "bt-hci op");
	if (error != 0) {
		kern_free(hci->acl_bytes);
		kern_free(hci->event_bytes);
		kern_free(hci);
		return error;
	}

	/* The queues, empty, and the reader's lock. */
	spin_init(&hci->lock, LOCK_RANK_DEVICE, "bt-hci");
	waitq_init(&hci->waitq, "bt-hci packet");
	bt_hci_ring_init(&hci->events, hci->event_bytes, BT_HCI_EVENT_RING, BT_HCI_EVENT_RESERVE);
	bt_hci_ring_init(&hci->acl, hci->acl_bytes, BT_HCI_ACL_RING, 0U);

	/* What the controller is, and the transport. */
	hci->info.vendor = description->vendor;
	hci->info.product = description->product;
	hci->info.version = description->version;
	hci->info.bus = description->bus;
	hci->info.acl_data_max = BT_ACL_DATA_DEFAULT;
	bt_hci_copy_text(hci->info.name, description->name);
	bt_hci_copy_text(hci->info.physical_path, description->physical_path);
	hci->ops = ops;
	hci->context = context;
	hci->registered = 1U;

	/* The lowest free number. */
	error = bt_hci_slot_claim(hci);
	if (error != 0) {
		kern_free(hci->acl_bytes);
		kern_free(hci->event_bytes);
		kern_free(hci);
		return error;
	}

	/* Publishes btN; the record lives as long as the node does. */
	kern_snprintf(node_name, sizeof(node_name), "bt%u", hci->number);
	error = cdev_register_managed(node_name,
	    (dev_t)(BT_HCI_DEVICE_BASE + hci->number),
	    &bt_hci_cdev_ops,
	    hci,
	    bt_hci_finalize,
	    &node);
	if (error != 0) {
		bt_hci_slot_release(hci);
		kern_free(hci->acl_bytes);
		kern_free(hci->event_bytes);
		kern_free(hci);
		return error;
	}

	/* The registration's reference, given back at the release. */
	hci->node = node;
	kern_logf("bt-hci: /dev/%s: %s %04x:%04x\n", node_name, hci->info.name, (unsigned)hci->info.vendor,
		  (unsigned)hci->info.product);

	/* Succeeded: the node is published and the record is the transport's handle. */
	*result = hci;
	return 0;
}

/*
 * Queues one packet the transport has received: an event or an ACL
 * packet, header first and without the type's byte.  Returns 0, EINVAL
 * for another type, or ENOSPC when its ring has no room now (the
 * transport keeps the packet and offers it again after the room call).
 * A packet of a withdrawn controller, or one nobody has open, is dropped.
 * Any context but an interrupt handler.
 */
int
drv_bt_hci_input(
	struct drv_bt_hci *hci,
	uint8_t type,
	const uint8_t *packet,
	size_t length)
{
	struct bt_hci_ring *ring;
	unsigned long irq;
	int error;

	/* The ring of the packet's type. */
	if (type == BT_PACKET_EVENT) {
		ring = &hci->events;
	} else if (type == BT_PACKET_ACL) {
		ring = &hci->acl;
	} else {
		return EINVAL;
	}

	/* Queues it with the next sequence number while somebody reads. */
	error = 0;
	irq = spin_lock_irqsave(&hci->lock);

	if (hci->registered && hci->opened) {
		error = bt_hci_ring_push(ring, type, hci->sequence, packet, length, 0);
		if (error == 0) {
			hci->sequence++;
			if (type == BT_PACKET_EVENT)
				hci->stats.events_in++;
			else
				hci->stats.acl_in++;
		}
	}

	spin_unlock_irqrestore(&hci->lock, irq);

	/* The reader waiting, and the pollers. */
	waitq_wake_all(&hci->waitq);
	poll_notify();

	/* Reports a packet the ring had no room for. */
	if (error != 0)
		return error;

	/* Succeeded: the packet is queued, or dropped with nobody to read it. */
	return 0;
}

/* Adds the transport's counts of malformed receptions and stalls to the controller's. */
void
drv_bt_hci_count(
	struct drv_bt_hci *hci,
	const struct drv_bt_hci_counts *counts)
{
	unsigned long irq;

	/* The counts under the lock. */
	irq = spin_lock_irqsave(&hci->lock);

	hci->stats.malformed += counts->malformed;
	hci->stats.stalls += counts->stalls;

	spin_unlock_irqrestore(&hci->lock, irq);
}

/*
 * Queues one of the kernel's notices (BT_PACKET_NOTICE_RESET) on the
 * events' ring, in the share kept for the notices (BT_HCI_EVENT_RESERVE:
 * four notices' records).  A notice for a ring full even of notices is
 * dropped (the program has not read four of them).
 */
void
drv_bt_hci_notice(
	struct drv_bt_hci *hci,
	uint8_t type)
{
	unsigned long irq;
	int error;

	/* Queues it with the next sequence number while somebody reads. */
	irq = spin_lock_irqsave(&hci->lock);

	if (hci->registered && hci->opened) {
		error = bt_hci_ring_push(&hci->events, type, hci->sequence, NULL, 0U, 1);
		if (error == 0)
			hci->sequence++;
	}

	spin_unlock_irqrestore(&hci->lock, irq);

	/* The reader waiting, and the pollers. */
	waitq_wake_all(&hci->waitq);
	poll_notify();
}

/*
 * Takes the transport away: the reader waiting answers ENODEV, no
 * operation and no room call runs after this returns, and later input is
 * dropped.  The node stays until drv_bt_hci_release().
 */
void
drv_bt_hci_withdraw(
	struct drv_bt_hci *hci)
{
	unsigned long irq;
	unsigned calls;

	/* Nothing was registered. */
	if (hci == NULL)
		return;

	/* registered tells the reader, the pollers and the room calls that the controller is gone. */
	irq = spin_lock_irqsave(&hci->lock);

	hci->registered = 0U;

	spin_unlock_irqrestore(&hci->lock, irq);

	/* The reader and the pollers see it now. */
	waitq_wake_all(&hci->waitq);
	poll_notify();

	/* Takes the transport away after the operation running now, if any. */
	mutex_lock(&hci->op_lock);

	hci->ops = NULL;
	hci->context = NULL;

	mutex_unlock(&hci->op_lock);

	/* Waits out a room call that began before registered was cleared (it does not sleep). */
	for (;;) {
		irq = spin_lock_irqsave(&hci->lock);

		calls = hci->room_calls;

		spin_unlock_irqrestore(&hci->lock, irq);

		/* None left: the transport is no longer called. */
		if (calls == 0U)
			break;
		sched_yield();
	}
}

/*
 * Unpublishes a withdrawn controller's node and gives back the
 * registration's reference.  The transport does not touch the record
 * after this.
 */
void
drv_bt_hci_release(
	struct drv_bt_hci *hci)
{
	struct cdev *node;

	/* Nothing was registered. */
	if (hci == NULL)
		return;

	/* The node goes first, so that a controller given the number next can publish bt<N> again. */
	node = hci->node;
	(void)cdev_unregister(node);

	/* The number may be given again; the registration's reference still keeps the record. */
	bt_hci_slot_release(hci);

	/* The registration's reference: the record may be freed now (the node's last reference), and is not touched after this. */
	cdev_release(node);
}

/* Opens a controller's node: the one open, with empty queues. */
static int
bt_hci_open(
	struct file *file)
{
	struct drv_bt_hci *hci;
	unsigned long irq;
	int error;

	/* The controller the node was opened on. */
	hci = bt_hci_file_device(file);
	if (hci == NULL)
		return ENODEV;

	/* Takes the node while it is there and free; what queued before the open is not this open's. */
	error = 0;
	irq = spin_lock_irqsave(&hci->lock);

	if (!hci->registered) {
		error = ENODEV;
	} else if (hci->opened) {
		error = EBUSY;
	} else {
		hci->opened = 1U;
		bt_hci_ring_clear(&hci->events);
		bt_hci_ring_clear(&hci->acl);
		file->f_data = hci;
	}

	spin_unlock_irqrestore(&hci->lock, irq);

	/* Reports a controller that is gone or held. */
	if (error != 0)
		return error;

	/* The transport may take packets again. */
	bt_hci_room_call(hci);

	/* Succeeded: the open sees every packet from now on. */
	return 0;
}

/* Closes the open: the node is free again and the packets not read are dropped. */
static int
bt_hci_close(
	struct file *file)
{
	struct drv_bt_hci *hci;
	unsigned long irq;

	/* The controller, when this file is its open. */
	hci = bt_hci_file_device(file);
	if (hci == NULL || file->f_data == NULL)
		return 0;

	/* Gives the node back and drops what was not read. */
	irq = spin_lock_irqsave(&hci->lock);

	hci->opened = 0U;
	bt_hci_ring_clear(&hci->events);
	bt_hci_ring_clear(&hci->acl);

	spin_unlock_irqrestore(&hci->lock, irq);

	/* The transport may take packets again (they are dropped until the next open). */
	file->f_data = NULL;
	bt_hci_room_call(hci);

	/* Succeeded: the open is closed. */
	return 0;
}

/*
 * Reads the oldest packet of the two queues, waiting for one unless the
 * file does not wait; a buffer shorter than it answers EMSGSIZE.
 */
static ssize_t
bt_hci_read(
	struct file *file,
	void *buffer,
	size_t size)
{
	struct drv_bt_hci *hci;
	struct bt_hci_ring *ring;
	unsigned long irq;
	uint64_t sequence;
	uint32_t record_sequence;
	size_t length;
	uint8_t type;
	int flags;
	int error;

	/* The controller of this open. */
	hci = bt_hci_file_device(file);
	if (hci == NULL || file->f_data == NULL)
		return -ENODEV;

	/* A read of nothing reads nothing (no packet is taken). */
	if (size == 0U)
		return 0;

	/* Waits until a packet is there, the controller goes, or a signal comes. */
	irq = spin_lock_irqsave(&hci->lock);

	for (;;) {
		/* A controller that is gone has no more packets. */
		if (!hci->registered) {
			spin_unlock_irqrestore(&hci->lock, irq);
			return -ENODEV;
		}

		/* The oldest packet: given whole, or left for a larger buffer. */
		ring = bt_hci_oldest(hci);
		if (ring != NULL) {
			(void)bt_hci_ring_peek(ring, &type, &record_sequence, &length);
			if (1U + length > size) {
				spin_unlock_irqrestore(&hci->lock, irq);
				return -EMSGSIZE;
			}

			/* The packet goes to the caller's buffer and leaves the queue. */
			length = bt_hci_ring_copy(ring, buffer);
			bt_hci_ring_pop(ring);
			break;
		}

		/* A file that does not wait. */
		flags = file_status_flags_get(file);
		if ((flags & O_NONBLOCK) != 0) {
			spin_unlock_irqrestore(&hci->lock, irq);
			return -EAGAIN;
		}

		/* Sleeps for the next packet. */
		sequence = waitq_sequence(&hci->waitq);
		error = waitq_sleep(&hci->waitq, &hci->lock, sequence, 0, WAITQ_INTERRUPTIBLE);
		if (error == EINTR) {
			spin_unlock_irqrestore(&hci->lock, irq);
			return -EINTR;
		}
	}

	spin_unlock_irqrestore(&hci->lock, irq);

	/* The room the packet left may let the transport take more. */
	bt_hci_room_call(hci);

	/* Succeeded: one whole packet with its type's byte. */
	return (ssize_t)length;
}

/* Sends one whole command or ACL packet through the transport. */
static ssize_t
bt_hci_write(
	struct file *file,
	const void *buffer,
	size_t size)
{
	struct drv_bt_hci *hci;
	unsigned long irq;
	size_t limit;
	int error;

	/* The controller of this open, and a packet no longer than the longest. */
	hci = bt_hci_file_device(file);
	if (hci == NULL || file->f_data == NULL)
		return -ENODEV;
	if (size > BT_ACL_PACKET_MAX)
		return -EINVAL;

	/* The ACL limit now. */
	irq = spin_lock_irqsave(&hci->lock);

	limit = hci->info.acl_data_max;

	spin_unlock_irqrestore(&hci->lock, irq);

	/* A whole packet of a carried type. */
	error = bt_hci_check_write(buffer, size, limit);
	if (error != 0)
		return -error;

	/* The transport sends it, one operation at a time. */
	error = mutex_lock_interruptible(&hci->op_lock);
	if (error != 0)
		return -EINTR;

	/* A transport that was taken away answers ENODEV. */
	error = ENODEV;
	if (hci->ops != NULL)
		error = hci->ops->send(hci->context, buffer, size);

	mutex_unlock(&hci->op_lock);

	/* Reports why the controller did not take it. */
	if (error != 0)
		return -error;

	/* The packet is counted as sent. */
	irq = spin_lock_irqsave(&hci->lock);

	if (((const uint8_t *)buffer)[0] == BT_PACKET_COMMAND)
		hci->stats.commands_out++;
	else
		hci->stats.acl_out++;

	spin_unlock_irqrestore(&hci->lock, irq);

	/* Succeeded: the whole packet was taken. */
	return (ssize_t)size;
}

/*
 * Answers the requests: the information, the counts, the bootloader's
 * path, the reset and the ACL limit.  The information and the counts are
 * given after the controller's withdrawal too (as they were last); the
 * requests that act on it answer ENODEV then.
 */
static int
bt_hci_ioctl(
	struct file *file,
	unsigned long request,
	uintptr_t argument)
{
	struct drv_bt_hci *hci;
	struct bt_info info;
	struct bt_stats stats;
	unsigned long irq;
	uint32_t value;
	int error;

	/* The controller of this open. */
	hci = bt_hci_file_device(file);
	if (hci == NULL || file->f_data == NULL)
		return ENODEV;

	/* Each request. */
	value = 0U;
	switch (request) {
	case BT_IOC_GET_INFO:
		/* What the controller is, with its flags and limit now. */
		irq = spin_lock_irqsave(&hci->lock);

		info = hci->info;

		spin_unlock_irqrestore(&hci->lock, irq);

		/* To the caller. */
		error = copyout(&info, argument, sizeof(info));
		break;
	case BT_IOC_GET_STATS:
		/* The counts now. */
		irq = spin_lock_irqsave(&hci->lock);

		stats = hci->stats;

		spin_unlock_irqrestore(&hci->lock, irq);

		/* To the caller. */
		error = copyout(&stats, argument, sizeof(stats));
		break;
	case BT_IOC_SET_BOOTLOADER:
	case BT_IOC_SET_ACL_MAX:
		/* A value, then the transport's or the class's part. */
		error = copyin(argument, &value, sizeof(value));
		if (error == 0) {
			/* The value came: the request acts. */
			error = bt_hci_operate(hci, request, value);
		}

		/* The value's request is done or refused. */
		break;
	case BT_IOC_RESET:
		/* The controller reset in place. */
		error = bt_hci_operate(hci, request, 0U);
		break;
	default:
		/* Anything else is not a controller's request. */
		return ENOTTY;
	}

	/* Reports why the request was not done. */
	if (error != 0)
		return error;

	/* Succeeded: the request is done. */
	return 0;
}

/* Says what the open can do now: read a packet, write, or nothing more (the controller is gone). */
static int
bt_hci_poll(
	struct file *file,
	short requested,
	short *returned)
{
	struct drv_bt_hci *hci;
	unsigned long irq;
	short result;

	/* The controller of this open. */
	if (returned == NULL)
		return EINVAL;
	hci = bt_hci_file_device(file);
	if (hci == NULL || file->f_data == NULL) {
		*returned = POLLERR | POLLHUP;
		return 0;
	}

	/* A packet waiting, a controller that takes packets, or one that is gone. */
	result = 0;
	irq = spin_lock_irqsave(&hci->lock);

	if (!hci->registered) {
		result |= POLLHUP;
	} else {
		if (hci->events.count != 0U || hci->acl.count != 0U)
			result |= requested & (POLLIN | POLLRDNORM);
		result |= requested & (POLLOUT | POLLWRNORM);
	}

	spin_unlock_irqrestore(&hci->lock, irq);

	/* Succeeded: the open's state. */
	*returned = result;
	return 0;
}

/* Finds the controller an open file is on. */
static struct drv_bt_hci *
bt_hci_file_device(
	struct file *file)
{
	const struct cdev *node;

	/* A file of a device node carries the device's generation. */
	if (file == NULL)
		return NULL;
	if (file->f_inode == NULL)
		return NULL;
	if (file->f_inode->i_data == NULL)
		return NULL;
	node = file->f_inode->i_data;

	/* The record the generation was published with. */
	return node->data;
}

/*
 * Picks the ring whose oldest record came first (the caller holds the
 * lock), or NULL when both are empty.
 */
static struct bt_hci_ring *
bt_hci_oldest(
	struct drv_bt_hci *hci)
{
	uint32_t event_sequence;
	uint32_t acl_sequence;
	size_t length;
	uint8_t type;
	int32_t difference;

	/* Only one ring, or none, has a record. */
	if (hci->events.count == 0U && hci->acl.count == 0U)
		return NULL;
	if (hci->acl.count == 0U)
		return &hci->events;
	if (hci->events.count == 0U)
		return &hci->acl;

	/* Both: the lower sequence number, by the sign of the wrapped difference. */
	(void)bt_hci_ring_peek(&hci->events, &type, &event_sequence, &length);
	(void)bt_hci_ring_peek(&hci->acl, &type, &acl_sequence, &length);
	difference = (int32_t)(event_sequence - acl_sequence);
	if (difference < 0)
		return &hci->events;

	/* The ACL packet came first. */
	return &hci->acl;
}

/*
 * Tells the transport that there is room again, while it is there.  The
 * call is counted so that the withdrawal can wait it out.
 */
static void
bt_hci_room_call(
	struct drv_bt_hci *hci)
{
	void (*room)(void *);
	unsigned long irq;
	void *context;

	/* The transport's room operation, while the controller is registered. */
	room = NULL;
	context = NULL;
	irq = spin_lock_irqsave(&hci->lock);

	if (hci->registered && hci->ops != NULL) {
		room = hci->ops->room;
		context = hci->context;
		hci->room_calls++;
	}

	spin_unlock_irqrestore(&hci->lock, irq);

	/* A withdrawn controller is not called. */
	if (room == NULL)
		return;

	/* The transport looks at its stopped transfers again. */
	room(context);

	/* The call is over; the withdrawal may go on. */
	irq = spin_lock_irqsave(&hci->lock);

	hci->room_calls--;

	spin_unlock_irqrestore(&hci->lock, irq);
}

/*
 * Does a request that acts on the controller or its limit, under the
 * operations' mutex: the bootloader's path, the reset, the ACL limit.
 */
static int
bt_hci_operate(
	struct drv_bt_hci *hci,
	unsigned long request,
	uint32_t value)
{
	unsigned long irq;
	int error;

	/* An ACL limit outside what a controller may have. */
	if (request == BT_IOC_SET_ACL_MAX) {
		if (value < BT_ACL_DATA_MIN || value > BT_ACL_DATA_MAX)
			return EINVAL;
	}

	/* The bootloader's path is turned on or off, nothing else. */
	if (request == BT_IOC_SET_BOOTLOADER) {
		if (value > 1U)
			return EINVAL;
	}

	/* One operation at a time, and none after the withdrawal. */
	error = mutex_lock_interruptible(&hci->op_lock);
	if (error != 0)
		return EINTR;
	if (hci->ops == NULL) {
		mutex_unlock(&hci->op_lock);
		return ENODEV;
	}

	/* The request's own part. */
	if (request == BT_IOC_SET_BOOTLOADER) {
		error = hci->ops->set_bootloader(hci->context, (int)value);
	} else if (request == BT_IOC_RESET) {
		error = hci->ops->reset(hci->context);
	} else {
		error = 0;
	}

	/* What the requests give out from now on. */
	if (error == 0) {
		irq = spin_lock_irqsave(&hci->lock);

		if (request == BT_IOC_SET_ACL_MAX)
			hci->info.acl_data_max = value;
		if (request == BT_IOC_SET_BOOTLOADER && value != 0U)
			hci->info.flags |= BT_INFO_BOOTLOADER;
		if (request == BT_IOC_SET_BOOTLOADER && value == 0U)
			hci->info.flags &= ~(uint32_t)BT_INFO_BOOTLOADER;

		spin_unlock_irqrestore(&hci->lock, irq);
	}

	mutex_unlock(&hci->op_lock);

	/* Reports why the controller did not do it. */
	if (error != 0)
		return error;

	/* Succeeded: the request is done. */
	return 0;
}

/* Gives a controller the lowest free number; returns 0 or ENOSPC. */
static int
bt_hci_slot_claim(
	struct drv_bt_hci *hci)
{
	unsigned long irq;
	unsigned number;
	int error;

	/* The first free slot. */
	error = ENOSPC;
	irq = spin_lock_irqsave(&bt_hci_registry_lock);

	for (number = 0U; number < DRV_BT_HCI_MAX; number++) {
		if (bt_hci_slots[number] == NULL) {
			bt_hci_slots[number] = hci;
			hci->number = number;
			error = 0;
			break;
		}
	}

	spin_unlock_irqrestore(&bt_hci_registry_lock, irq);

	/* Reports that every number is taken. */
	if (error != 0)
		return error;

	/* Succeeded: the controller holds its number. */
	return 0;
}

/* Gives a controller's number back. */
static void
bt_hci_slot_release(
	struct drv_bt_hci *hci)
{
	unsigned long irq;

	/* The slot is free when it still holds this controller. */
	irq = spin_lock_irqsave(&bt_hci_registry_lock);

	if (hci->number < DRV_BT_HCI_MAX && bt_hci_slots[hci->number] == hci)
		bt_hci_slots[hci->number] = NULL;

	spin_unlock_irqrestore(&bt_hci_registry_lock, irq);
}

/* Frees a controller's record with the last reference of its node. */
static void
bt_hci_finalize(
	void *data)
{
	struct drv_bt_hci *hci;

	/* The rings and the record. */
	hci = data;
	kern_free(hci->acl_bytes);
	kern_free(hci->event_bytes);
	kern_free(hci);
}

/* Copies a text into a field of BT_TEXT_MAX bytes, cut and ended by a NUL. */
static void
bt_hci_copy_text(
	char *target,
	const char *source)
{
	size_t length;

	/* Nothing is an empty text. */
	target[0] = '\0';
	if (source == NULL)
		return;

	/* As much as fits. */
	length = kern_strlen(source);
	if (length >= BT_TEXT_MAX)
		length = BT_TEXT_MAX - 1U;
	kern_memcpy(target, source, length);
	target[length] = '\0';
}
