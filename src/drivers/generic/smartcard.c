/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The smart card slots' class (drivers/generic/smartcard.h, ws161-p003).
 *
 * Each registered slot gets the lowest free number N and the character
 * device smartcardN (devfs: /dev/smartcardN, mode 0600; sessiond gives it
 * to the seat's user).  Any number of files may be open on it; each gets
 * a queue of card events.  CCID_POWER_ON gives the slot's claim to the
 * file that asked, and only that file may then power the card or send it
 * APDUs, until CCID_POWER_OFF or its last close, which powers the card
 * off.  The transport's operations run one at a time for the slot under
 * the operations' mutex; the withdrawal takes the transport away under the
 * same mutex, so no operation runs after it returns.  The state, the claim
 * and the event queues are guarded by the slot's spinlock.  The record
 * goes with the device's last reference.
 */

#include <drivers/generic/smartcard.h>

#include "kern/cdev.h"
#include "kern/clock.h"
#include "kern/file.h"
#include "kern/klog.h"
#include "kern/kmem.h"
#include "kern/lock.h"
#include "kern/poll.h"
#include "kern/uaccess.h"
#include "kern/waitq.h"
#include <kern/kcrt.h>
#include <uapi/errno.h>
#include <uapi/fcntl.h>
#include <uapi/poll.h>
#include <uapi/system.h>

/* The device numbers of the class: one major, the minor is N. */
#define SMARTCARD_DEVICE_BASE	0x00100000U

/*
 * One open of a slot: its ring of card events not yet read (the oldest at
 * head, count of them).  Guarded by the slot's spinlock.
 */
struct smartcard_reader {
	struct smartcard_reader *next;
	struct ccid_event events[CCID_EVENT_QUEUE];
	unsigned head;
	unsigned count;
};

/* One published slot. */
struct drv_smartcard {
	/*
	 * Guards the readers and their rings, the state, the ATR, the change
	 * count, the claim and registered.  The readers sleep on waitq under it.
	 */
	struct spinlock lock;
	struct wait_queue waitq;
	struct smartcard_reader *readers;
	uint32_t state;
	uint8_t atr[CCID_ATR_MAX];
	uint32_t atr_size;
	uint32_t changes;
	struct file *claim;
	unsigned registered;

	/*
	 * Serializes the transport's operations with each other and with the
	 * withdrawal, which clears ops: an operation that finds ops NULL
	 * answers ENODEV.
	 */
	struct mutex ops_lock;
	const struct drv_smartcard_ops *ops;
	void *context;

	/* What the requests give out; fixed at registration. */
	struct ccid_info info;

	/* The slot the device holds and the device's reference the registration owns. */
	unsigned number;
	struct cdev *node;
};

/*
 * The system's events, posted when a slot comes and goes.  Weak, so a
 * kernel without the events links; a null test there posts nothing.
 */
extern void kern_system_event_post(uint32_t, uint32_t, int32_t, const char *, const char *) __attribute__((weak));

static int smartcard_open(struct file *file);
static int smartcard_close(struct file *file);
static ssize_t smartcard_read(struct file *file, void *buffer, size_t size);
static int smartcard_ioctl(struct file *file, unsigned long request, uintptr_t argument);
static int smartcard_poll(struct file *file, short requested, short *returned);
static int smartcard_get_info(struct drv_smartcard *smartcard, struct file *file, uintptr_t argument);
static int smartcard_get_status(struct drv_smartcard *smartcard, uintptr_t argument);
static int smartcard_power_on(struct drv_smartcard *smartcard, struct file *file, uintptr_t argument);
static int smartcard_power_off(struct drv_smartcard *smartcard, struct file *file);
static int smartcard_transmit(struct drv_smartcard *smartcard, struct file *file, uintptr_t argument);
static int smartcard_claimed_by(struct drv_smartcard *smartcard, struct file *file);
static int smartcard_powered(struct drv_smartcard *smartcard);
static void smartcard_release_claim(struct drv_smartcard *smartcard, struct file *file);
static void smartcard_event(struct drv_smartcard *smartcard, uint32_t kind);
static struct drv_smartcard *smartcard_file_device(struct file *file);
static int smartcard_slot_claim(struct drv_smartcard *smartcard);
static void smartcard_slot_release(struct drv_smartcard *smartcard);
static void smartcard_finalize(void *data);
static void smartcard_post(const struct drv_smartcard *smartcard, uint32_t action);

/* The operations of every slot. */
static const struct cdev_ops smartcard_cdev_ops = {
	.open = smartcard_open,
	.close = smartcard_close,
	.read = smartcard_read,
	.ioctl = smartcard_ioctl,
	.poll = smartcard_poll
};

/* The slot holding each number, NULL for a free one; guarded by smartcard_registry_lock. */
static struct drv_smartcard *smartcard_slots[DRV_SMARTCARD_MAX];

/* Serializes the claiming and the release of the numbers for the kernel's life. */
static struct spinlock smartcard_registry_lock = {
	{ 0 }, LOCK_RANK_DEVICE, "smartcard registry", 0, 0
};

/*
 * Registers a slot and publishes it.
 *
 * Returns 0 with the slot's record in *result, EINVAL for a missing part
 * or a slot that takes no APDU, ENOSPC when every number is taken,
 * ENOMEM, or the error of publishing the device.
 */
int
drv_smartcard_register(
	const struct drv_smartcard_description *description,
	const struct drv_smartcard_ops *ops,
	void *context,
	struct drv_smartcard **result)
{
	struct drv_smartcard *smartcard;
	struct cdev *node;
	char node_name[32];
	int error;

	/* A transport describes its slot, does the three operations, and the slot takes an APDU. */
	if (description == NULL || ops == NULL || result == NULL)
		return EINVAL;
	if (ops->power_on == NULL || ops->power_off == NULL || ops->transmit == NULL)
		return EINVAL;
	if (description->info.max_command < 5U || description->info.max_response < 2U)
		return EINVAL;

	/* The slot's record. */
	smartcard = kern_calloc(1U, sizeof(*smartcard));
	if (smartcard == NULL)
		return ENOMEM;

	/* The operations' lock. */
	error = mutex_init(&smartcard->ops_lock, LOCK_RANK_DEVICE, "smartcard ops");
	if (error != 0) {
		kern_free(smartcard);
		return error;
	}

	/* The transport, what the slot is, whether a card is there, and the readers' lock. */
	spin_init(&smartcard->lock, LOCK_RANK_DEVICE, "smartcard");
	waitq_init(&smartcard->waitq, "smartcard event");
	smartcard->ops = ops;
	smartcard->context = context;
	smartcard->info = description->info;
	smartcard->info.name[CCID_TEXT_MAX - 1U] = '\0';
	smartcard->state = CCID_CARD_ABSENT;
	if (description->present)
		smartcard->state = CCID_CARD_PRESENT;
	smartcard->registered = 1U;

	/* The lowest free number. */
	error = smartcard_slot_claim(smartcard);
	if (error != 0) {
		kern_free(smartcard);
		return error;
	}

	/* Publishes smartcardN; the record lives as long as the device does. */
	kern_snprintf(node_name, sizeof(node_name), "smartcard%u", smartcard->number);
	error = cdev_register_managed(node_name,
	    (dev_t)(SMARTCARD_DEVICE_BASE + smartcard->number),
	    &smartcard_cdev_ops,
	    smartcard,
	    smartcard_finalize,
	    &node);
	if (error != 0) {
		smartcard_slot_release(smartcard);
		kern_free(smartcard);
		return error;
	}

	/* The registration's reference, given back at the withdrawal. */
	smartcard->node = node;
	kern_logf("smartcard: /dev/%s: %s slot %u of %u\n", node_name, smartcard->info.name,
		  (unsigned)smartcard->info.slot, (unsigned)smartcard->info.slot_count);

	/* The desktop hears the new slot. */
	smartcard_post(smartcard, KERN_SYSTEM_EVENT_ADD);

	/* Succeeded: the slot is published and the record is the transport's handle. */
	*result = smartcard;
	return 0;
}

/*
 * Tells the class that a card came into the slot or went from it.  A card
 * that went takes its power and its ATR with it; the claim stays with its
 * file, whose next APDU answers ENXIO.  Any context but an interrupt
 * handler.
 */
void
drv_smartcard_card(
	struct drv_smartcard *smartcard,
	int present)
{
	unsigned long irq;
	uint32_t kind;
	int changed;

	/* Nothing to tell. */
	if (smartcard == NULL)
		return;

	/* The state, when it changes. */
	changed = 0;
	kind = CCID_EVENT_REMOVED;
	irq = spin_lock_irqsave(&smartcard->lock);

	if (present && smartcard->state == CCID_CARD_ABSENT) {
		smartcard->state = CCID_CARD_PRESENT;
		kind = CCID_EVENT_INSERTED;
		changed = 1;
	} else if (!present && smartcard->state != CCID_CARD_ABSENT) {
		smartcard->state = CCID_CARD_ABSENT;
		smartcard->atr_size = 0U;
		kern_memset(smartcard->atr, 0, sizeof(smartcard->atr));
		changed = 1;
	}

	spin_unlock_irqrestore(&smartcard->lock, irq);

	/* Nothing changed: nothing to tell. */
	if (!changed)
		return;

	/* Every open hears the change, and the desktop too (ws199-p001: a key held to an NFC reader). */
	smartcard_event(smartcard, kind);
	smartcard_post(smartcard, KERN_SYSTEM_EVENT_CHANGE);
}

/*
 * Withdraws a slot: the readers waiting answer ENODEV, no operation runs
 * after this returns, and the node goes.
 */
void
drv_smartcard_unregister(
	struct drv_smartcard *smartcard)
{
	struct cdev *node;
	unsigned long irq;

	/* Nothing was registered. */
	if (smartcard == NULL)
		return;

	/* registered tells the readers and the pollers that the slot is gone. */
	irq = spin_lock_irqsave(&smartcard->lock);

	smartcard->registered = 0U;
	smartcard->state = CCID_CARD_ABSENT;

	spin_unlock_irqrestore(&smartcard->lock, irq);
	waitq_wake_all(&smartcard->waitq);
	poll_notify();

	/* Takes the transport away after the operation running now, if any. */
	mutex_lock(&smartcard->ops_lock);

	smartcard->ops = NULL;
	smartcard->context = NULL;

	mutex_unlock(&smartcard->ops_lock);

	/* The desktop hears the slot go, and the number may be given again. */
	smartcard_post(smartcard, KERN_SYSTEM_EVENT_REMOVE);
	smartcard_slot_release(smartcard);

	/*
	 * Unpublishes the device and gives back the registration's reference;
	 * the record may be freed by then (the device's last reference), so it
	 * is not touched after this.
	 */
	node = smartcard->node;
	(void)cdev_unregister(node);
	cdev_release(node);
}

/* Opens a slot: a reader with an empty queue of events. */
static int
smartcard_open(
	struct file *file)
{
	struct drv_smartcard *smartcard;
	struct smartcard_reader *reader;
	unsigned long irq;
	int attached;

	/* The slot the node was opened on. */
	smartcard = smartcard_file_device(file);
	if (smartcard == NULL)
		return ENODEV;

	/* The reader. */
	reader = kern_calloc(1U, sizeof(*reader));
	if (reader == NULL)
		return ENOMEM;

	/* Joins the slot's readers while it is there. */
	attached = 0;
	irq = spin_lock_irqsave(&smartcard->lock);

	if (smartcard->registered) {
		reader->next = smartcard->readers;
		smartcard->readers = reader;
		file->f_data = reader;
		attached = 1;
	}

	spin_unlock_irqrestore(&smartcard->lock, irq);

	/* A slot withdrawn meanwhile. */
	if (!attached) {
		kern_free(reader);
		return ENODEV;
	}

	/* Succeeded: the open hears every change from now on. */
	return 0;
}

/* Closes an open: a claim it holds is given up (the card is powered off), and its reader goes. */
static int
smartcard_close(
	struct file *file)
{
	struct drv_smartcard *smartcard;
	struct smartcard_reader *reader;
	struct smartcard_reader **link;
	unsigned long irq;

	/* The slot and the open's reader. */
	smartcard = smartcard_file_device(file);
	reader = file->f_data;
	if (smartcard == NULL || reader == NULL)
		return 0;

	/* No security state of the card passes to the next program. */
	smartcard_release_claim(smartcard, file);

	/* Takes the reader off the slot's list. */
	irq = spin_lock_irqsave(&smartcard->lock);

	for (link = &smartcard->readers; *link != NULL; link = &(*link)->next) {
		if (*link == reader) {
			*link = reader->next;
			break;
		}
	}

	spin_unlock_irqrestore(&smartcard->lock, irq);

	/* The reader goes. */
	file->f_data = NULL;
	kern_free(reader);

	/* Succeeded: the open is closed. */
	return 0;
}

/* Reads whole event records, waiting for the first unless the file does not wait. */
static ssize_t
smartcard_read(
	struct file *file,
	void *buffer,
	size_t size)
{
	struct drv_smartcard *smartcard;
	struct smartcard_reader *reader;
	struct ccid_event *events;
	unsigned long irq;
	uint64_t sequence;
	size_t capacity;
	size_t count;
	int flags;
	int error;

	/* The slot, the open's reader, and room for one record at least. */
	smartcard = smartcard_file_device(file);
	reader = file->f_data;
	if (smartcard == NULL || reader == NULL)
		return -ENODEV;
	if (size < sizeof(struct ccid_event))
		return -EINVAL;
	capacity = size / sizeof(struct ccid_event);
	events = buffer;

	/* Waits until a record is there, the slot goes, or a signal comes. */
	irq = spin_lock_irqsave(&smartcard->lock);

	for (;;) {
		/* As many records as fit, oldest first. */
		if (reader->count != 0U) {
			count = 0U;
			while (count < capacity && reader->count != 0U) {
				events[count] = reader->events[reader->head];
				reader->head = (reader->head + 1U) % CCID_EVENT_QUEUE;
				reader->count--;
				count++;
			}

			/* Succeeded: the records read. */
			spin_unlock_irqrestore(&smartcard->lock, irq);
			return (ssize_t)(count * sizeof(struct ccid_event));
		}

		/* A slot that is gone has no more records. */
		if (!smartcard->registered) {
			spin_unlock_irqrestore(&smartcard->lock, irq);
			return -ENODEV;
		}

		/* A file that does not wait. */
		flags = file_status_flags_get(file);
		if ((flags & O_NONBLOCK) != 0) {
			spin_unlock_irqrestore(&smartcard->lock, irq);
			return -EAGAIN;
		}

		/* Sleeps for the next change. */
		sequence = waitq_sequence(&smartcard->waitq);
		error = waitq_sleep(&smartcard->waitq, &smartcard->lock, sequence, 0, WAITQ_INTERRUPTIBLE);
		if (error == EINTR) {
			spin_unlock_irqrestore(&smartcard->lock, irq);
			return -EINTR;
		}
	}
}

/* Answers the requests. */
static int
smartcard_ioctl(
	struct file *file,
	unsigned long request,
	uintptr_t argument)
{
	struct drv_smartcard *smartcard;
	int error;

	/* The slot the node was opened on. */
	smartcard = smartcard_file_device(file);
	if (smartcard == NULL || file->f_data == NULL)
		return ENODEV;

	/* Each request. */
	switch (request) {
	case CCID_GET_INFO:
		error = smartcard_get_info(smartcard, file, argument);
		break;
	case CCID_GET_STATUS:
		error = smartcard_get_status(smartcard, argument);
		break;
	case CCID_POWER_ON:
		error = smartcard_power_on(smartcard, file, argument);
		break;
	case CCID_POWER_OFF:
		error = smartcard_power_off(smartcard, file);
		break;
	case CCID_TRANSMIT:
		error = smartcard_transmit(smartcard, file, argument);
		break;
	default:
		/* Anything else is not a slot's request. */
		return ENOTTY;
	}

	/* Reports why the request failed. */
	if (error != 0)
		return error;

	/* Succeeded: the request is answered. */
	return 0;
}

/* Says what an open can do now: read an event, or nothing more (the slot is gone). */
static int
smartcard_poll(
	struct file *file,
	short requested,
	short *returned)
{
	struct drv_smartcard *smartcard;
	struct smartcard_reader *reader;
	unsigned long irq;
	short result;

	/* The slot and the open's reader. */
	if (returned == NULL)
		return EINVAL;
	smartcard = smartcard_file_device(file);
	reader = file->f_data;
	if (smartcard == NULL || reader == NULL) {
		*returned = POLLERR | POLLHUP;
		return 0;
	}

	/* A record waiting, or a slot that is gone. */
	result = 0;
	irq = spin_lock_irqsave(&smartcard->lock);

	if (reader->count != 0U)
		result |= requested & (POLLIN | POLLRDNORM);
	if (!smartcard->registered)
		result |= POLLHUP;

	spin_unlock_irqrestore(&smartcard->lock, irq);

	/* Succeeded: the open's state. */
	*returned = result;
	return 0;
}

/* Gives out what the slot is, with whether this open holds the claim. */
static int
smartcard_get_info(
	struct drv_smartcard *smartcard,
	struct file *file,
	uintptr_t argument)
{
	struct ccid_info info;
	int claimed;
	int error;

	/* The registration's description, and this open's claim. */
	info = smartcard->info;
	claimed = smartcard_claimed_by(smartcard, file);
	if (claimed)
		info.flags |= CCID_INFO_CLAIMED;

	/* Hands it to the caller. */
	error = copyout(&info, argument, sizeof(info));
	if (error != 0)
		return error;

	/* Succeeded: the caller has the description. */
	return 0;
}

/* Gives out the slot's state: the card, its ATR while powered, and the change count. */
static int
smartcard_get_status(
	struct drv_smartcard *smartcard,
	uintptr_t argument)
{
	struct ccid_status status;
	unsigned long irq;
	int error;

	/* Samples the state. */
	kern_memset(&status, 0, sizeof(status));
	irq = spin_lock_irqsave(&smartcard->lock);

	status.state = smartcard->state;
	status.atr_size = smartcard->atr_size;
	kern_memcpy(status.atr, smartcard->atr, sizeof(status.atr));
	status.changes = smartcard->changes;

	spin_unlock_irqrestore(&smartcard->lock, irq);

	/* Hands it to the caller. */
	error = copyout(&status, argument, sizeof(status));
	if (error != 0)
		return error;

	/* Succeeded: the caller has the state. */
	return 0;
}

/*
 * Claims the slot for this open (another open's claim: EBUSY), powers the
 * card (no card: ENXIO, and the claim is given back) and gives its ATR.
 */
static int
smartcard_power_on(
	struct drv_smartcard *smartcard,
	struct file *file,
	uintptr_t argument)
{
	struct ccid_status status;
	unsigned long irq;
	uint8_t atr[CCID_ATR_MAX];
	uint64_t deadline;
	size_t atr_size;
	int newly;
	int error;

	/* The claim: free, or this open's already. */
	newly = 0;
	error = 0;
	irq = spin_lock_irqsave(&smartcard->lock);

	if (smartcard->claim == NULL) {
		smartcard->claim = file;
		newly = 1;
	} else if (smartcard->claim != file) {
		error = EBUSY;
	}

	spin_unlock_irqrestore(&smartcard->lock, irq);
	if (error != 0)
		return error;

	/* The transport powers the card (a transport taken away answers ENODEV). */
	atr_size = 0U;
	deadline = clock_milliseconds(NULL) + CCID_TRANSMIT_DEFAULT_MS;
	mutex_lock(&smartcard->ops_lock);

	error = ENODEV;
	if (smartcard->ops != NULL) {
		error = smartcard->ops->power_on(
			smartcard->context,
			smartcard->info.slot,
			atr,
			&atr_size,
			deadline);
	}

	mutex_unlock(&smartcard->ops_lock);

	/* A card that did not power gives the claim back when it was taken now. */
	if (error != 0) {
		if (newly) {
			irq = spin_lock_irqsave(&smartcard->lock);
			if (smartcard->claim == file)
				smartcard->claim = NULL;
			spin_unlock_irqrestore(&smartcard->lock, irq);
		}

		/* Reports why the card is not powered. */
		return error;
	}

	/* The powered card and its ATR. */
	if (atr_size > CCID_ATR_MAX)
		atr_size = CCID_ATR_MAX;
	kern_memset(&status, 0, sizeof(status));
	irq = spin_lock_irqsave(&smartcard->lock);

	smartcard->state = CCID_CARD_POWERED;
	kern_memset(smartcard->atr, 0, sizeof(smartcard->atr));
	kern_memcpy(smartcard->atr, atr, atr_size);
	smartcard->atr_size = (uint32_t)atr_size;
	status.state = smartcard->state;
	status.atr_size = smartcard->atr_size;
	kern_memcpy(status.atr, smartcard->atr, sizeof(status.atr));
	status.changes = smartcard->changes;

	spin_unlock_irqrestore(&smartcard->lock, irq);

	/* Hands the state to the caller. */
	error = copyout(&status, argument, sizeof(status));
	if (error != 0)
		return error;

	/* Succeeded: the card is powered for this open. */
	return 0;
}

/* Powers the card off and gives up the claim (only the claim's holder). */
static int
smartcard_power_off(
	struct drv_smartcard *smartcard,
	struct file *file)
{
	int claimed;

	/* Only the holder of the claim. */
	claimed = smartcard_claimed_by(smartcard, file);
	if (!claimed)
		return EPERM;

	/* The card's power and the claim go. */
	smartcard_release_claim(smartcard, file);

	/* Succeeded: the slot is free. */
	return 0;
}

/*
 * Sends one command APDU from the caller and gives back the answer (only
 * the claim's holder).  The command and the answer go through kernel
 * buffers of their own.
 */
static int
smartcard_transmit(
	struct drv_smartcard *smartcard,
	struct file *file,
	uintptr_t argument)
{
	struct ccid_transmit transmit;
	uint8_t *command;
	uint8_t *response;
	uint64_t deadline;
	size_t response_size;
	uint32_t timeout;
	int claimed;
	int powered;
	int error;

	/* Only the holder of the claim, with the card powered (a card that went: ENXIO). */
	claimed = smartcard_claimed_by(smartcard, file);
	if (!claimed)
		return EPERM;
	powered = smartcard_powered(smartcard);
	if (!powered)
		return ENXIO;

	/* The request. */
	error = copyin(argument, &transmit, sizeof(transmit));
	if (error != 0)
		return error;
	if (transmit.command_size < 4U || transmit.command_size > smartcard->info.max_command)
		return EINVAL;
	if (transmit.response_capacity < 2U)
		return EINVAL;
	if (transmit.response_capacity > smartcard->info.max_response)
		transmit.response_capacity = smartcard->info.max_response;

	/* The wait: the default, or the caller's up to the longest. */
	timeout = transmit.timeout_ms;
	if (timeout == 0U)
		timeout = CCID_TRANSMIT_DEFAULT_MS;
	if (timeout > CCID_TRANSMIT_MAX_MS)
		timeout = CCID_TRANSMIT_MAX_MS;

	/* The command's copy. */
	command = kern_malloc(transmit.command_size);
	if (command == NULL)
		return ENOMEM;
	error = copyin((uintptr_t)transmit.command, command, transmit.command_size);
	if (error != 0) {
		kern_free(command);
		return error;
	}

	/* The answer's room. */
	response = kern_malloc(transmit.response_capacity);
	if (response == NULL) {
		kern_free(command);
		return ENOMEM;
	}

	/* The transport sends it, by the deadline. */
	response_size = 0U;
	deadline = clock_milliseconds(NULL) + timeout;
	mutex_lock(&smartcard->ops_lock);

	error = ENODEV;
	if (smartcard->ops != NULL) {
		error = smartcard->ops->transmit(
			smartcard->context,
			smartcard->info.slot,
			command,
			transmit.command_size,
			response,
			transmit.response_capacity,
			&response_size,
			deadline);
	}

	mutex_unlock(&smartcard->ops_lock);

	/* The answer, to the caller. */
	if (error == 0) {
		error = copyout(response, (uintptr_t)transmit.response, response_size);
		transmit.response_size = (uint32_t)response_size;
	}

	/* The request with the answer's size. */
	if (error == 0)
		error = copyout(&transmit, argument, sizeof(transmit));

	/* The command and the answer do not stay in the kernel. */
	kern_memset(command, 0, transmit.command_size);
	kern_memset(response, 0, transmit.response_capacity);
	kern_free(command);
	kern_free(response);
	if (error != 0)
		return error;

	/* Succeeded: the caller has the answer. */
	return 0;
}

/* Tells whether this open holds the slot's claim. */
static int
smartcard_claimed_by(
	struct drv_smartcard *smartcard,
	struct file *file)
{
	unsigned long irq;
	int claimed;

	/* Samples the claim. */
	irq = spin_lock_irqsave(&smartcard->lock);

	claimed = 0;
	if (smartcard->claim == file)
		claimed = 1;

	spin_unlock_irqrestore(&smartcard->lock, irq);

	/* Reports whether it is this open's. */
	return claimed;
}

/* Tells whether the slot's card is powered. */
static int
smartcard_powered(
	struct drv_smartcard *smartcard)
{
	unsigned long irq;
	int powered;

	/* Samples the state. */
	irq = spin_lock_irqsave(&smartcard->lock);

	powered = 0;
	if (smartcard->state == CCID_CARD_POWERED)
		powered = 1;

	spin_unlock_irqrestore(&smartcard->lock, irq);

	/* Reports whether it is powered. */
	return powered;
}

/* Gives up this open's claim, powering the card off first (nothing when it holds none). */
static void
smartcard_release_claim(
	struct drv_smartcard *smartcard,
	struct file *file)
{
	unsigned long irq;
	int claimed;

	/* Only a claim this open holds. */
	claimed = smartcard_claimed_by(smartcard, file);
	if (!claimed)
		return;

	/* The card's power goes first, so the next holder starts from a fresh card. */
	mutex_lock(&smartcard->ops_lock);

	if (smartcard->ops != NULL)
		(void)smartcard->ops->power_off(smartcard->context, smartcard->info.slot);

	mutex_unlock(&smartcard->ops_lock);

	/* The claim, and the ATR of the card that is no longer powered. */
	irq = spin_lock_irqsave(&smartcard->lock);

	if (smartcard->claim == file)
		smartcard->claim = NULL;
	if (smartcard->state == CCID_CARD_POWERED)
		smartcard->state = CCID_CARD_PRESENT;
	smartcard->atr_size = 0U;
	kern_memset(smartcard->atr, 0, sizeof(smartcard->atr));

	spin_unlock_irqrestore(&smartcard->lock, irq);
}

/* Counts a change and gives every open a record of it (a full queue loses its oldest). */
static void
smartcard_event(
	struct drv_smartcard *smartcard,
	uint32_t kind)
{
	struct smartcard_reader *reader;
	unsigned long irq;
	unsigned slot;

	/* The count and the records. */
	irq = spin_lock_irqsave(&smartcard->lock);

	smartcard->changes++;
	for (reader = smartcard->readers; reader != NULL; reader = reader->next) {
		/* A full queue gives up its oldest record. */
		if (reader->count == CCID_EVENT_QUEUE) {
			reader->head = (reader->head + 1U) % CCID_EVENT_QUEUE;
			reader->count--;
		}

		/* The record. */
		slot = (reader->head + reader->count) % CCID_EVENT_QUEUE;
		reader->events[slot].kind = kind;
		reader->events[slot].changes = smartcard->changes;
		reader->count++;
	}

	spin_unlock_irqrestore(&smartcard->lock, irq);

	/* The readers waiting, and the pollers. */
	waitq_wake_all(&smartcard->waitq);
	poll_notify();
}

/* Finds the slot an open file is on. */
static struct drv_smartcard *
smartcard_file_device(
	struct file *file)
{
	const struct cdev *node;

	/* A file of a device node carries the device's generation. */
	if (file == NULL || file->f_inode == NULL || file->f_inode->i_data == NULL)
		return NULL;
	node = file->f_inode->i_data;

	/* The record the generation was published with. */
	return node->data;
}

/* Gives a slot the lowest free number; returns 0 or ENOSPC. */
static int
smartcard_slot_claim(
	struct drv_smartcard *smartcard)
{
	unsigned long irq;
	unsigned number;
	int error;

	/* The first free number. */
	error = ENOSPC;
	irq = spin_lock_irqsave(&smartcard_registry_lock);

	for (number = 0U; number < DRV_SMARTCARD_MAX; number++) {
		if (smartcard_slots[number] == NULL) {
			smartcard_slots[number] = smartcard;
			smartcard->number = number;
			error = 0;
			break;
		}
	}

	spin_unlock_irqrestore(&smartcard_registry_lock, irq);

	/* Reports whether a number was free. */
	return error;
}

/* Gives a slot's number back. */
static void
smartcard_slot_release(
	struct drv_smartcard *smartcard)
{
	unsigned long irq;

	/* The number is free when it still names this slot. */
	irq = spin_lock_irqsave(&smartcard_registry_lock);

	if (smartcard->number < DRV_SMARTCARD_MAX && smartcard_slots[smartcard->number] == smartcard)
		smartcard_slots[smartcard->number] = NULL;

	spin_unlock_irqrestore(&smartcard_registry_lock, irq);
}

/* Frees a slot's record with the last reference of its node. */
static void
smartcard_finalize(
	void *data)
{
	/* The record. */
	kern_free(data);
}

/*
 * Tells the system's events that a slot came or went, or that its card
 * did (CHANGE, ws199-p001), in the class of the USB devices; the detail
 * says whether a card is in the slot (card=, before the name, which may
 * be cut short).
 */
static void
smartcard_post(
	const struct drv_smartcard *smartcard,
	uint32_t action)
{
	char subject[KERN_SYSTEM_EVENT_SUBJECT_MAX];
	char detail[KERN_SYSTEM_EVENT_DETAIL_MAX];

	/* A kernel without the system's events posts nothing. */
	if (kern_system_event_post == NULL)
		return;

	/* The node, the detail, then the event. */
	kern_snprintf(subject, sizeof(subject), "smartcard%u", smartcard->number);
	kern_snprintf(detail, sizeof(detail), "vendor=%04x product=%04x slot=%u card=%u name=%s",
		      (unsigned)smartcard->info.vendor, (unsigned)smartcard->info.product,
		      (unsigned)smartcard->info.slot, (unsigned)(smartcard->state != CCID_CARD_ABSENT),
		      smartcard->info.name);
	kern_system_event_post(KERN_SYSTEM_EVENT_USB, action, 0, subject, detail);
}
