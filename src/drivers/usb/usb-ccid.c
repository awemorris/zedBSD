/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The USB CCID reader driver (ws161-p003; the slots' interface is
 * <uapi/ccid.h>, docs/reference/security-keys.md).
 *
 * It takes an interface of class 0x0B (Smart Card, CCID 1.1) with a bulk
 * OUT and a bulk IN endpoint whose reader exchanges whole APDUs (dwFeatures),
 * and publishes each of its slots (up to USB_CCID_SLOTS) through the smart
 * card class.  The reader frames the APDUs for the card; for an NFC reader
 * such as the ACR1252U the card is a contactless one (ISO 14443-4) that the
 * reader shows as a powered ICC.
 *
 * One command is outstanding at a time for the whole reader: every command
 * (from the class's operations and from the worker) takes the command
 * mutex, sends its message on bulk OUT with the next bSeq, and reads bulk
 * IN until the answer of that bSeq comes (an older answer, left by an
 * aborted command, is dropped).  An answer that says the card asks for more
 * time is waited past, but never past the command's deadline; at the
 * deadline the command is aborted at the reader (the ABORT class request
 * and PC_to_RDR_Abort).  A long answer the reader sends in parts
 * (bChainParameter) is collected here.
 *
 * The worker asks every slot's state every USB_CCID_POLL_MS (GetSlotStatus,
 * under the command mutex) and tells the class when a card comes or goes.
 * The detach stops the worker, withdraws the slots (the class waits for an
 * operation running), and then frees the reader.
 */

#include <drivers/generic/smartcard.h>
#include <drivers/usb/usb-ccid.h>
#include <drivers/usb/usb.h>

#include <kern/clock.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <uapi/errno.h>

/* The most slots of one reader published, and how often the worker asks their state. */
#define USB_CCID_SLOTS			4U
#define USB_CCID_POLL_MS		500U

/* The longest the worker's status question, a power-off and an abort may take. */
#define USB_CCID_STATUS_MS		2000U
#define USB_CCID_ABORT_MS		1000U

/* The largest message buffer (the USB core's largest transfer), and the shortest a reader may have. */
#define USB_CCID_BUFFER_MAX		(64U * 1024U)
#define USB_CCID_BUFFER_MIN		(CCID_HEADER + 261U)

/* The longest short APDU command and answer, and the longest extended answer. */
#define USB_CCID_SHORT_COMMAND		261U
#define USB_CCID_SHORT_RESPONSE		258U
#define USB_CCID_EXTENDED_RESPONSE	65538U

/* bPowerSelect: the reader chooses, or 1.8 V, 3 V and 5 V, tried in that order. */
#define USB_CCID_POWER_AUTOMATIC	0U
#define USB_CCID_POWER_1V8		3U
#define USB_CCID_POWER_3V		2U
#define USB_CCID_POWER_5V		1U

/*
 * One reader.
 *
 * command_lock serializes the commands, and with them sequence, out and
 * in; stopping tells the worker to leave.  slots[] are the class's
 * records, published at attach and withdrawn at detach; present[] is what
 * the worker last told the class.
 */
struct usb_ccid {
	struct drv_usb_interface *interface;
	struct drv_usb_device *device;
	struct drv_usb_endpoint *bulk_in;
	struct drv_usb_endpoint *bulk_out;
	struct ccid_class class;
	int level;
	unsigned slot_count;
	size_t buffer_size;
	struct mutex command_lock;
	uint8_t sequence;
	uint8_t *out;
	uint8_t *in;
	struct thread *worker;
	volatile unsigned stopping;
	struct drv_smartcard *slots[USB_CCID_SLOTS];
	int present[USB_CCID_SLOTS];
	char name[CCID_TEXT_MAX];
};

static int usb_ccid_match(struct drv_usb_interface *interface, const struct drv_usb_id *id);
static int usb_ccid_attach(struct drv_usb_interface *interface, const struct drv_usb_id *id);
static int usb_ccid_detach(struct drv_usb_interface *interface, unsigned flags);
static int usb_ccid_find_class(struct drv_usb_interface *interface, struct ccid_class *result);
static int usb_ccid_publish(struct usb_ccid *reader);
static void usb_ccid_withdraw(struct usb_ccid *reader);
static void usb_ccid_free(struct usb_ccid *reader);
static void usb_ccid_worker(void *argument);
static int usb_ccid_power_on(void *context, unsigned slot, uint8_t *atr, size_t *atr_size, uint64_t deadline_ms);
static int usb_ccid_power_off(void *context, unsigned slot);
static int usb_ccid_transmit(void *context, unsigned slot, const uint8_t *command, size_t command_size, uint8_t *response, size_t capacity, size_t *response_size, uint64_t deadline_ms);
static int usb_ccid_slot_present(struct usb_ccid *reader, unsigned slot, int *present);
static int usb_ccid_exchange(struct usb_ccid *reader, uint8_t type, unsigned slot, const uint8_t *data, size_t length, uint8_t parameter0, uint8_t parameter1, uint8_t parameter2, uint64_t deadline_ms, struct ccid_reply *reply);
static void usb_ccid_abort(struct usb_ccid *reader, unsigned slot, uint8_t sequence);
static unsigned usb_ccid_remaining(uint64_t deadline_ms);
static int usb_ccid_card_error(const struct ccid_reply *reply);

/* The interfaces the driver takes: class 0x0B. */
static const struct drv_usb_id usb_ccid_ids[] = {
	{
		.match_flags = DRV_USB_ID_IF_CLASS,
		.interface_class = CCID_INTERFACE_CLASS
	}
};

/* The driver, registered once at boot. */
static struct drv_usb_driver usb_ccid_driver = {
	.name = "usb-ccid",
	.ids = usb_ccid_ids,
	.id_count = sizeof(usb_ccid_ids) / sizeof(usb_ccid_ids[0]),
	.match = usb_ccid_match,
	.attach = usb_ccid_attach,
	.detach = usb_ccid_detach
};

/* What a reader's slot does for the smart card class. */
static const struct drv_smartcard_ops usb_ccid_ops = {
	.power_on = usb_ccid_power_on,
	.power_off = usb_ccid_power_off,
	.transmit = usb_ccid_transmit
};

/*
 * Registers the driver with the USB core.
 */
int
drv_usb_ccid_driver_register(
	void)
{
	int error;

	/* The core matches it against every interface from now on. */
	error = drv_usb_driver_register(&usb_ccid_driver);
	if (error != 0)
		return error;

	/* Succeeded: the driver is registered. */
	return 0;
}

/* Takes an interface whose reader exchanges whole APDUs on a pair of bulk endpoints. */
static int
usb_ccid_match(
	struct drv_usb_interface *interface,
	const struct drv_usb_id *id)
{
	struct drv_usb_endpoint *bulk_in;
	struct drv_usb_endpoint *bulk_out;
	struct ccid_class class;
	int level;
	int error;

	/* The class descriptor. */
	(void)id;
	error = usb_ccid_find_class(interface, &class);
	if (error != 0)
		return 0;

	/* A reader of whole APDUs (TPDU and character readers are left alone). */
	level = drv_ccid_level(class.features);
	if (level == CCID_LEVEL_NONE)
		return 0;

	/* Its two bulk endpoints. */
	bulk_in = drv_usb_interface_find_endpoint(interface, DRV_USB_TRANSFER_BULK, DRV_USB_DIR_IN, NULL);
	bulk_out = drv_usb_interface_find_endpoint(interface, DRV_USB_TRANSFER_BULK, DRV_USB_DIR_OUT, NULL);
	if (bulk_in == NULL || bulk_out == NULL)
		return 0;

	/* This driver takes it. */
	return 100;
}

/* Binds a reader: its description, its buffers, its slots and its worker. */
static int
usb_ccid_attach(
	struct drv_usb_interface *interface,
	const struct drv_usb_id *id)
{
	const struct drv_usb_device_descriptor *descriptor;
	struct usb_ccid *reader;
	const char *level;
	int error;

	/* The reader's record. */
	(void)id;
	reader = kern_calloc(1U, sizeof(*reader));
	if (reader == NULL)
		return ENOMEM;
	reader->interface = interface;
	reader->device = drv_usb_interface_device(interface);

	/* Its class descriptor, its exchange level and its endpoints. */
	error = usb_ccid_find_class(interface, &reader->class);
	if (error != 0) {
		kern_free(reader);
		return error;
	}

	/* A reader of whole APDUs on two bulk endpoints. */
	reader->level = drv_ccid_level(reader->class.features);
	reader->bulk_in = drv_usb_interface_find_endpoint(interface, DRV_USB_TRANSFER_BULK, DRV_USB_DIR_IN, NULL);
	reader->bulk_out = drv_usb_interface_find_endpoint(interface, DRV_USB_TRANSFER_BULK, DRV_USB_DIR_OUT, NULL);
	if (reader->level == CCID_LEVEL_NONE || reader->bulk_in == NULL || reader->bulk_out == NULL) {
		kern_free(reader);
		return ENODEV;
	}

	/* Its slots, as many as are published. */
	reader->slot_count = (unsigned)reader->class.max_slot_index + 1U;
	if (reader->slot_count > USB_CCID_SLOTS)
		reader->slot_count = USB_CCID_SLOTS;

	/* A message as long as the reader takes, within the USB core's largest transfer. */
	reader->buffer_size = reader->class.max_message;
	if (reader->buffer_size > USB_CCID_BUFFER_MAX)
		reader->buffer_size = USB_CCID_BUFFER_MAX;
	if (reader->buffer_size < USB_CCID_BUFFER_MIN) {
		kern_logf("usb-ccid: reader's longest message %u is too short\n", (unsigned)reader->class.max_message);
		kern_free(reader);
		return ENODEV;
	}

	/* The command's lock and the two buffers. */
	error = mutex_init(&reader->command_lock, LOCK_RANK_DEVICE, "usb-ccid");
	if (error != 0) {
		kern_free(reader);
		return error;
	}

	/* The two message buffers. */
	reader->out = kern_malloc(reader->buffer_size);
	reader->in = kern_malloc(reader->buffer_size);
	if (reader->out == NULL || reader->in == NULL) {
		usb_ccid_free(reader);
		return ENOMEM;
	}

	/* The product's name. */
	descriptor = drv_usb_device_descriptor(reader->device);
	kern_snprintf(reader->name, sizeof(reader->name), "USB smart card reader");
	if (descriptor->product_string != 0U) {
		(void)drv_usb_device_get_string(reader->device, descriptor->product_string, 0,
		    reader->name, sizeof(reader->name));
	}

	/* The slots, published with their cards' state now. */
	error = usb_ccid_publish(reader);
	if (error != 0) {
		usb_ccid_withdraw(reader);
		usb_ccid_free(reader);
		return error;
	}

	/* The worker that watches the cards. */
	error = drv_usb_interface_set_driver_data(interface, reader);
	if (error == 0)
		error = kthread_create(usb_ccid_worker, reader, SCHED_PRIORITY_DEFAULT, &reader->worker);
	if (error != 0) {
		(void)drv_usb_interface_set_driver_data(interface, NULL);
		usb_ccid_withdraw(reader);
		usb_ccid_free(reader);
		return error;
	}

	/* The worker runs. */
	thread_start(reader->worker);

	/* Succeeded: the reader is in service. */
	level = "short";
	if (reader->level == CCID_LEVEL_EXTENDED)
		level = "extended";
	kern_logf("usb-ccid: %s: %u slot(s), %s APDUs, messages of %u bytes\n", reader->name, reader->slot_count,
		  level, (unsigned)reader->buffer_size);
	return 0;
}

/* Gives the reader up: its worker stops, its slots go, and its record. */
static int
usb_ccid_detach(
	struct drv_usb_interface *interface,
	unsigned flags)
{
	struct usb_ccid *reader;
	struct thread *worker;
	unsigned state;
	int error;

	/* The reader the interface had. */
	(void)flags;
	reader = drv_usb_interface_driver_data(interface);
	if (reader == NULL)
		return 0;

	/* The worker stops (its last question ends within its time). */
	reader->stopping = 1U;
	worker = reader->worker;
	if (worker != NULL) {
		if (worker == curthread)
			return EBUSY;

		/* Wakes the worker so it sees the stop request. */
		sched_notify_task(worker->task);
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
		reader->worker = NULL;
	}

	/* The slots go (the class waits for an operation running), then the record. */
	usb_ccid_withdraw(reader);
	(void)drv_usb_interface_set_driver_data(interface, NULL);
	usb_ccid_free(reader);

	/* Succeeded: the reader is given up. */
	return 0;
}

/* Finds and reads an interface's CCID class descriptor among its extra descriptors. */
static int
usb_ccid_find_class(
	struct drv_usb_interface *interface,
	struct ccid_class *result)
{
	const struct drv_usb_host_interface *alternate;
	const uint8_t *descriptor;
	size_t length;
	unsigned count;
	unsigned index;
	int error;

	/* The active setting's extra descriptors. */
	alternate = drv_usb_interface_active_alternate(interface);
	if (alternate == NULL)
		return ENODEV;
	count = drv_usb_host_interface_extra_count(alternate);

	/* The first of the class's type. */
	for (index = 0U; index < count; index++) {
		error = drv_usb_host_interface_extra(alternate, index, (const void **)&descriptor, &length);
		if (error != 0)
			return error;
		if (length < 2U || descriptor[1] != CCID_CLASS_DESCRIPTOR)
			continue;

		/* Read as the CCID class descriptor. */
		error = drv_ccid_parse_class(descriptor, length, result);
		return error;
	}

	/* An interface without one is no CCID reader. */
	return ENODEV;
}

/* Publishes every slot through the smart card class, with whether a card is in it. */
static int
usb_ccid_publish(
	struct usb_ccid *reader)
{
	const struct drv_usb_device_descriptor *usb_descriptor;
	struct drv_smartcard_description description;
	unsigned slot;
	int present;
	int error;

	/* What every slot shares. */
	usb_descriptor = drv_usb_device_descriptor(reader->device);
	kern_memset(&description, 0, sizeof(description));
	description.info.vendor = usb_descriptor->vendor;
	description.info.product = usb_descriptor->product;
	description.info.version = usb_descriptor->device_release;
	description.info.interface_number = (uint16_t)drv_usb_interface_number(reader->interface);
	description.info.slot_count = (uint8_t)reader->slot_count;
	description.info.features = reader->class.features;
	description.info.protocols = reader->class.protocols;
	description.info.max_message = reader->class.max_message;
	kern_memcpy(description.info.name, reader->name, sizeof(description.info.name));

	/* The longest command and answer: short APDUs, or what a message holds and a chain collects. */
	description.info.max_command = USB_CCID_SHORT_COMMAND;
	description.info.max_response = USB_CCID_SHORT_RESPONSE;
	if (reader->level == CCID_LEVEL_EXTENDED) {
		description.info.max_command = (uint32_t)(reader->buffer_size - CCID_HEADER);
		description.info.max_response = USB_CCID_EXTENDED_RESPONSE;
		description.info.flags |= CCID_INFO_EXTENDED_APDU;
	}

	/* Each slot, with the card's state now. */
	for (slot = 0U; slot < reader->slot_count; slot++) {
		present = 0;
		(void)usb_ccid_slot_present(reader, slot, &present);
		reader->present[slot] = present;
		description.info.slot = (uint8_t)slot;
		description.present = present;
		error = drv_smartcard_register(&description, &usb_ccid_ops, reader, &reader->slots[slot]);
		if (error != 0)
			return error;
	}

	/* Succeeded: every slot is published. */
	return 0;
}

/* Withdraws every published slot. */
static void
usb_ccid_withdraw(
	struct usb_ccid *reader)
{
	unsigned slot;

	/* Each slot that was published. */
	for (slot = 0U; slot < USB_CCID_SLOTS; slot++) {
		if (reader->slots[slot] == NULL)
			continue;
		drv_smartcard_unregister(reader->slots[slot]);
		reader->slots[slot] = NULL;
	}
}

/* Frees the reader's buffers and its record. */
static void
usb_ccid_free(
	struct usb_ccid *reader)
{
	/* The buffers and the record. */
	if (reader->out != NULL)
		kern_free(reader->out);
	if (reader->in != NULL)
		kern_free(reader->in);
	kern_free(reader);
}

/* Watches the cards: asks every slot's state now and then, and tells the class of a change. */
static void
usb_ccid_worker(
	void *argument)
{
	struct usb_ccid *reader;
	unsigned slot;
	int present;
	int error;

	/* Until the detach. */
	reader = argument;
	while (!reader->stopping) {
		/* Each slot. */
		for (slot = 0U; slot < reader->slot_count && !reader->stopping; slot++) {
			/* Its state; a reader that does not answer is asked again next time. */
			error = usb_ccid_slot_present(reader, slot, &present);
			if (error != 0)
				continue;
			if (present == reader->present[slot])
				continue;

			/* The class hears the change. */
			reader->present[slot] = present;
			drv_smartcard_card(reader->slots[slot], present);
		}

		/* The next round. */
		if (!reader->stopping)
			sched_sleep(sched_ticks() + kern_ms_to_ticks(USB_CCID_POLL_MS));
	}
}

/* Powers a slot's card and gives its ATR (the class's operation). */
static int
usb_ccid_power_on(
	void *context,
	unsigned slot,
	uint8_t *atr,
	size_t *atr_size,
	uint64_t deadline_ms)
{
	static const uint8_t voltages[3] = { USB_CCID_POWER_1V8, USB_CCID_POWER_3V, USB_CCID_POWER_5V };
	struct usb_ccid *reader;
	struct ccid_reply reply;
	unsigned tries;
	unsigned index;
	uint8_t voltage;
	int error;

	/* The reader chooses the voltage, or the lowest that works is tried first. */
	reader = context;
	tries = 3U;
	if ((reader->class.features & CCID_FEATURE_AUTO_VOLTAGE) != 0U)
		tries = 1U;

	/* Each voltage, until the card answers with its ATR. */
	error = ENXIO;
	mutex_lock(&reader->command_lock);

	for (index = 0U; index < tries; index++) {
		/* PC_to_RDR_IccPowerOn with the voltage. */
		voltage = voltages[index];
		if (tries == 1U)
			voltage = USB_CCID_POWER_AUTOMATIC;
		error = usb_ccid_exchange(reader, CCID_PC_TO_RDR_ICC_POWER_ON, slot, NULL, 0U, voltage, 0U, 0U,
		    deadline_ms, &reply);
		if (error != 0)
			break;

		/* The ATR, or why the card did not answer. */
		error = usb_ccid_card_error(&reply);
		if (error == 0) {
			*atr_size = reply.length;
			if (*atr_size > CCID_ATR_MAX)
				*atr_size = CCID_ATR_MAX;
			kern_memcpy(atr, reply.data, *atr_size);
			break;
		}

		/* A card that failed at this voltage is tried at the next. */
		if (error != EIO)
			break;
	}

	mutex_unlock(&reader->command_lock);

	/* Reports why the card is not powered. */
	if (error != 0)
		return error;

	/* Succeeded: the card is powered. */
	return 0;
}

/* Powers a slot's card off (the class's operation). */
static int
usb_ccid_power_off(
	void *context,
	unsigned slot)
{
	struct usb_ccid *reader;
	struct ccid_reply reply;
	int error;

	/* PC_to_RDR_IccPowerOff. */
	reader = context;
	mutex_lock(&reader->command_lock);

	error = usb_ccid_exchange(reader, CCID_PC_TO_RDR_ICC_POWER_OFF, slot, NULL, 0U, 0U, 0U, 0U,
	    clock_milliseconds(NULL) + USB_CCID_STATUS_MS, &reply);

	mutex_unlock(&reader->command_lock);

	/* Reports a reader that did not answer. */
	if (error != 0)
		return error;

	/* Succeeded: the card is off. */
	return 0;
}

/*
 * Sends one command APDU and collects the answer, in parts when the reader
 * chains it (the class's operation).
 */
static int
usb_ccid_transmit(
	void *context,
	unsigned slot,
	const uint8_t *command,
	size_t command_size,
	uint8_t *response,
	size_t capacity,
	size_t *response_size,
	uint64_t deadline_ms)
{
	struct usb_ccid *reader;
	struct ccid_reply reply;
	size_t collected;
	int more;
	int error;

	/* A command that fits a message. */
	reader = context;
	if (command_size > reader->buffer_size - CCID_HEADER)
		return EMSGSIZE;

	/* PC_to_RDR_XfrBlock with the command, then the next parts while the answer continues. */
	collected = 0U;
	mutex_lock(&reader->command_lock);

	error = usb_ccid_exchange(reader, CCID_PC_TO_RDR_XFR_BLOCK, slot, command, command_size, 0U, 0U, 0U,
	    deadline_ms, &reply);
	for (;;) {
		/* A failed exchange, or a card that failed it. */
		if (error != 0)
			break;
		error = usb_ccid_card_error(&reply);
		if (error != 0)
			break;

		/* This part of the answer, when it fits. */
		if (reply.length > capacity - collected) {
			error = EMSGSIZE;
			break;
		}

		/* Kept after the parts before it. */
		kern_memcpy(response + collected, reply.data, reply.length);
		collected += reply.length;

		/* A part that says more follows asks for it. */
		more = 0;
		if (reply.parameter == CCID_CHAIN_BEGINS ||
		    reply.parameter == CCID_CHAIN_CONTINUES ||
		    reply.parameter == CCID_CHAIN_EMPTY)
			more = 1;
		if (!more)
			break;
		error = usb_ccid_exchange(reader, CCID_PC_TO_RDR_XFR_BLOCK, slot, NULL, 0U, 0U,
		    (uint8_t)(CCID_LEVEL_CONTINUE & 0xffU), (uint8_t)(CCID_LEVEL_CONTINUE >> 8U), deadline_ms, &reply);
	}

	mutex_unlock(&reader->command_lock);

	/* Reports why there is no whole answer. */
	if (error != 0)
		return error;

	/* Succeeded: the whole answer. */
	*response_size = collected;
	return 0;
}

/* Asks whether a card is in a slot (GetSlotStatus). */
static int
usb_ccid_slot_present(
	struct usb_ccid *reader,
	unsigned slot,
	int *present)
{
	struct ccid_reply reply;
	int error;

	/* PC_to_RDR_GetSlotStatus. */
	mutex_lock(&reader->command_lock);

	error = usb_ccid_exchange(reader, CCID_PC_TO_RDR_GET_SLOT_STATUS, slot, NULL, 0U, 0U, 0U, 0U,
	    clock_milliseconds(NULL) + USB_CCID_STATUS_MS, &reply);

	mutex_unlock(&reader->command_lock);
	if (error != 0)
		return error;

	/* Present unless the card's state says there is none. */
	*present = 1;
	if ((reply.status & 0x03U) == CCID_ICC_ABSENT)
		*present = 0;

	/* Succeeded: the slot's state. */
	return 0;
}

/*
 * Sends one command and reads its answer (the caller holds the command
 * mutex): the answers of other bSeq are dropped, an answer asking for more
 * time is waited past, and at the deadline the command is aborted.  The
 * answer's data stays in the reader's in buffer until the next command.
 */
static int
usb_ccid_exchange(
	struct usb_ccid *reader,
	uint8_t type,
	unsigned slot,
	const uint8_t *data,
	size_t length,
	uint8_t parameter0,
	uint8_t parameter1,
	uint8_t parameter2,
	uint64_t deadline_ms,
	struct ccid_reply *reply)
{
	size_t actual;
	unsigned remaining;
	uint8_t sequence;
	int error;

	/* The message: the header with the next bSeq, then the data. */
	reader->sequence++;
	sequence = reader->sequence;
	drv_ccid_header(reader->out, type, (uint32_t)length, (uint8_t)slot, sequence, parameter0, parameter1, parameter2);
	if (length != 0U)
		kern_memcpy(reader->out + CCID_HEADER, data, length);

	/* Sends it. */
	remaining = usb_ccid_remaining(deadline_ms);
	if (remaining == 0U)
		return ETIMEDOUT;
	actual = 0U;
	error = drv_usb_bulk(reader->device, reader->bulk_out, reader->out, CCID_HEADER + length, remaining, &actual);
	if (error == 0 && actual != CCID_HEADER + length)
		error = EIO;
	if (error != 0)
		return error;

	/* Reads answers until this command's own, final one. */
	for (;;) {
		/* The next answer, by the deadline. */
		remaining = usb_ccid_remaining(deadline_ms);
		if (remaining == 0U) {
			usb_ccid_abort(reader, slot, sequence);
			return ETIMEDOUT;
		}

		/* Reads it; a reader that stays silent past the deadline gets the command aborted. */
		actual = 0U;
		error = drv_usb_bulk(reader->device, reader->bulk_in, reader->in, reader->buffer_size, remaining, &actual);
		if (error == ETIMEDOUT) {
			usb_ccid_abort(reader, slot, sequence);
			return ETIMEDOUT;
		}

		/* A reader that is gone, or a transfer that failed. */
		if (error != 0)
			return error;

		/* A whole answer of this command; another command's (an aborted one's) is dropped. */
		error = drv_ccid_parse_reply(reader->in, actual, reply);
		if (error != 0)
			return error;
		if (reply->sequence != sequence || reply->slot != (uint8_t)slot)
			continue;

		/* The card asks for more time: the next answer is waited for. */
		if ((reply->status >> 6U) == CCID_COMMAND_TIME_EXTENSION)
			continue;

		/* Succeeded: this command's answer. */
		return 0;
	}
}

/*
 * Aborts a command at the reader: the ABORT class request, then
 * PC_to_RDR_Abort of the same bSeq, and its answer (best effort; a reader
 * that does not answer leaves an answer the next command drops).
 */
static void
usb_ccid_abort(
	struct usb_ccid *reader,
	unsigned slot,
	uint8_t sequence)
{
	struct ccid_reply reply;
	uint64_t deadline;
	unsigned remaining;
	size_t actual;
	int error;

	/* The class request on the control pipe. */
	actual = 0U;
	(void)drv_usb_control(reader->device,
	    DRV_USB_DIR_OUT | DRV_USB_REQUEST_CLASS | DRV_USB_RECIP_INTERFACE,
	    CCID_REQUEST_ABORT,
	    (uint16_t)(((unsigned)sequence << 8U) | slot),
	    (uint16_t)drv_usb_interface_number(reader->interface),
	    NULL,
	    0U,
	    USB_CCID_ABORT_MS,
	    &actual);

	/* PC_to_RDR_Abort with the same bSeq. */
	drv_ccid_header(reader->out, CCID_PC_TO_RDR_ABORT, 0U, (uint8_t)slot, sequence, 0U, 0U, 0U);
	error = drv_usb_bulk(reader->device, reader->bulk_out, reader->out, CCID_HEADER, USB_CCID_ABORT_MS, &actual);
	if (error != 0)
		return;

	/* Its answer, or the aborted command's; anything else is dropped. */
	deadline = clock_milliseconds(NULL) + USB_CCID_ABORT_MS;
	for (;;) {
		remaining = usb_ccid_remaining(deadline);
		if (remaining == 0U)
			return;
		error = drv_usb_bulk(reader->device, reader->bulk_in, reader->in, reader->buffer_size, remaining, &actual);
		if (error != 0)
			return;
		error = drv_ccid_parse_reply(reader->in, actual, &reply);
		if (error == 0 && reply.sequence == sequence && reply.type == CCID_RDR_TO_PC_SLOT_STATUS)
			return;
	}
}

/* Gives the milliseconds left before a deadline (0 when it has passed). */
static unsigned
usb_ccid_remaining(
	uint64_t deadline_ms)
{
	uint64_t now;

	/* Now, against the deadline. */
	now = clock_milliseconds(NULL);
	if (now >= deadline_ms)
		return 0U;

	/* What is left, as a transfer's timeout. */
	return (unsigned)(deadline_ms - now);
}

/*
 * Tells why an answer carries no card's data: 0 for a command that went
 * well, ENXIO for a card that is not there, EIO for one that failed it.
 */
static int
usb_ccid_card_error(
	const struct ccid_reply *reply)
{
	/* No card in the slot. */
	if ((reply->status & 0x03U) == CCID_ICC_ABSENT)
		return ENXIO;

	/* The command failed at the card or the reader. */
	if ((reply->status >> 6U) == CCID_COMMAND_FAILED)
		return EIO;

	/* The answer carries the card's data. */
	return 0;
}
