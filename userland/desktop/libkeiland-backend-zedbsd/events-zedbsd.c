/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The system's events on zedBSD (ws132-p003): the compositor's backend
 * subscribes to /dev/system's POWER, LID, AC, BATTERY, INPUT and USB
 * classes (ws132-p002) and tells the compositor through the host's
 * callbacks.
 *
 * The descriptor is opened nonblocking when the backend opens and joins
 * the event loop's poll.  Each poll that finds it readable reads every
 * record waiting, in reads of a few records, until none is left:
 *   - INPUT (a device came or went): input_changed, once a read;
 *   - AC and BATTERY: power_changed, once a read;
 *   - POWER's PRESS: power_button with the button the subject names,
 *     except a power button's release (events_power_release);
 *   - LID: lid_changed with the record's value (1 open, 0 closed);
 *   - a security key that came or went (ws199-p001): an INPUT record of a
 *     FIDO node (its detail's usage=f1d0:0001), or a USB record of a smart
 *     card slot (smartcardN: the slot, or its card): keys_changed, once a
 *     read;
 *   - OVERFLOW (records were lost): input_changed, power_changed and
 *     keys_changed, which make the compositor look at the devices, the
 *     power and the keys again.
 * A kernel without the events (the subscription refused) and a descriptor
 * that fails are closed, and nothing more is heard; the compositor's own
 * scans still find input devices.
 *
 * The devices that come and go (INPUT and USB) are heard on a thread of
 * the compositor's instead once it starts (kl_backend_devices_start): a
 * second descriptor subscribes to them, and the event loop's keeps POWER,
 * LID, AC and BATTERY.  The thread waits on it without a time limit
 * (kl_backend_devices_wait), opens each input device that comes (eventN,
 * the record's subject) through the session manager's seat socket when
 * there is one (input-zedbsd.c), and hands the event loop the devices it
 * opened with what the records asked: the refusals forgotten, keys_changed,
 * and for lost records input_changed, which looks at every device again.
 */

#include "userland/desktop/libkeiland-backend/backend-private.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <uapi/system.h>

/* The node of the system device. */
#define EVENTS_NODE	"/dev/system"

/* The classes the compositor hears. */
#define EVENTS_CLASSES	(KERN_SYSTEM_EVENT_POWER | KERN_SYSTEM_EVENT_LID | KERN_SYSTEM_EVENT_AC | \
			 KERN_SYSTEM_EVENT_BATTERY | KERN_SYSTEM_EVENT_INPUT | KERN_SYSTEM_EVENT_USB)

/* The classes of the devices that come and go, which the devices' thread hears once it runs. */
#define EVENTS_DEVICE_CLASSES	(KERN_SYSTEM_EVENT_INPUT | KERN_SYSTEM_EVENT_USB)

/* The evdev nodes' directory, and their names' start (an INPUT record's subject). */
#define EVENTS_INPUT_DIRECTORY	"/dev/input"
#define EVENTS_EVDEV_PREFIX	"event"

/* A FIDO node's usage in an INPUT record's detail, and a smart card slot's subject (ws199-p001). */
#define EVENTS_FIDO_USAGE	"usage=f1d0:0001"
#define EVENTS_CARD_SUBJECT	"smartcard"

/* The records one read takes. */
#define EVENTS_READ	8U

/*
 * The time after a power button's press within which the next power
 * button's record is taken as its release (milliseconds, WS182).
 */
#define EVENTS_RELEASE_MS	2000U

/* What one pass of records asks of the compositor once, after them. */
#define EVENTS_INPUT	1U
#define EVENTS_POWER	2U
#define EVENTS_KEYS	4U

static unsigned events_dispatch(struct kl_backend *backend, const struct system_event *event);
static int events_power_release(struct kl_backend *backend, const struct system_event *event);
static void events_fail(struct kl_backend *backend, const char *what, int error);
static int events_subscribe(int descriptor, uint32_t classes);
static int events_devices_read(struct kl_backend *backend, struct kl_backend_device_batch *batch);
static void events_device(struct kl_backend *backend, const struct system_event *event, struct kl_backend_device_batch *batch);
static void events_input_came(struct kl_backend *backend, const char *subject, struct kl_backend_device_batch *batch);

/*
 * Opens /dev/system and subscribes to the compositor's classes.  A system
 * that does not have it, or a kernel that refuses the subscription, leaves
 * the descriptor -1.
 */
void
kl_backend_events_open(
	struct kl_backend *backend)
{
	int descriptor;
	int error;

	/* No devices' thread yet, and nothing cancelled. */
	backend->devices_descriptor = -1;
	backend->cancel[0] = -1;
	backend->cancel[1] = -1;

	/* The pipe that ends the waits for good as the compositor stops, kept from its children. */
	error = pipe(backend->cancel);
	if (error != 0) {
		printf("KL EVENTS cancel unavailable errno=%d\n", errno);
		backend->cancel[0] = -1;
		backend->cancel[1] = -1;
	} else {
		(void)fcntl(backend->cancel[0], F_SETFD, FD_CLOEXEC);
		(void)fcntl(backend->cancel[1], F_SETFD, FD_CLOEXEC);
	}

	/* One request at a time on the seat socket. */
	error = pthread_mutex_init(&backend->seat_lock, NULL);
	if (error == 0)
		backend->seat_lock_ready = 1U;

	/* The device, read without waiting and kept from the compositor's children. */
	descriptor = open(EVENTS_NODE, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (descriptor < 0) {
		printf("KL EVENTS unavailable errno=%d\n", errno);
		return;
	}

	/* The classes; an older kernel refuses them. */
	error = events_subscribe(descriptor, EVENTS_CLASSES);
	if (error != 0) {
		printf("KL EVENTS unavailable errno=%d\n", error);
		(void)close(descriptor);
		return;
	}

	/* Succeeded: the events are heard from now on. */
	backend->events_descriptor = descriptor;
	printf("KL EVENTS subscribed classes=0x%x\n", (unsigned)EVENTS_CLASSES);
}

/*
 * Tells whether the backend tells every input device that comes or goes:
 * yes while /dev/system's events are heard (a kernel without them, or a
 * failed read, leaves the compositor's own scans).
 */
int
kl_backend_input_events(
	const struct kl_backend *backend)
{
	/* Nothing heard without a subscription. */
	if (backend == NULL)
		return 0;

	/* The devices' thread opens the devices that come. */
	if (backend->devices_descriptor >= 0)
		return 1;

	/* Nothing heard without the event loop's subscription either. */
	if (backend->events_descriptor < 0)
		return 0;

	/* Succeeded: INPUT and OVERFLOW call input_changed. */
	return 1;
}

/* Closes the events' descriptors, the cancel pipe and the seat socket's lock. */
void
kl_backend_events_close(
	struct kl_backend *backend)
{
	/* The devices' descriptor (its thread has ended). */
	if (backend->devices_descriptor >= 0) {
		(void)close(backend->devices_descriptor);
		backend->devices_descriptor = -1;
	}

	/* The cancel pipe. */
	if (backend->cancel[0] >= 0) {
		(void)close(backend->cancel[0]);
		(void)close(backend->cancel[1]);
		backend->cancel[0] = -1;
		backend->cancel[1] = -1;
	}

	/* The seat socket's lock. */
	if (backend->seat_lock_ready) {
		(void)pthread_mutex_destroy(&backend->seat_lock);
		backend->seat_lock_ready = 0U;
	}

	/* Nothing open. */
	if (backend->events_descriptor < 0)
		return;

	/* Closed; nothing more is heard. */
	(void)close(backend->events_descriptor);
	backend->events_descriptor = -1;
}

/*
 * Subscribes a second descriptor to the devices that come and go for the
 * devices' thread, and takes them off the event loop's.
 */
int
kl_backend_devices_start(
	struct kl_backend *backend)
{
	int descriptor;
	int error;

	/* A kernel without the events tells no device (the findings look for them). */
	if (backend == NULL || backend->events_descriptor < 0)
		return ENOTSUP;

	/* The second descriptor, polled by the thread, read without waiting, kept from the compositor's children. */
	descriptor = open(EVENTS_NODE, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (descriptor < 0)
		return errno;

	/* The devices' classes. */
	error = events_subscribe(descriptor, EVENTS_DEVICE_CLASSES);
	if (error != 0) {
		(void)close(descriptor);
		return error;
	}

	/* The thread's from now on. */
	backend->devices_descriptor = descriptor;

	/* The event loop keeps the machine's own (a refusal leaves both hearing the devices: twice is harmless). */
	error = events_subscribe(backend->events_descriptor, EVENTS_CLASSES & ~EVENTS_DEVICE_CLASSES);
	if (error != 0)
		printf("KL DEVICES the event loop still hears the devices errno=%d\n", error);

	/* Succeeded: the thread hears the devices from now on. */
	printf("KL DEVICES subscribed classes=0x%x\n", (unsigned)EVENTS_DEVICE_CLASSES);
	return 0;
}

/*
 * Waits without a time limit for the devices' records and fills batch with
 * what they asked: the input devices that came are opened.
 */
int
kl_backend_devices_wait(
	struct kl_backend *backend,
	struct kl_backend_device_batch *batch)
{
	struct pollfd entries[2];
	nfds_t count;
	int ready;
	int error;

	/* Nothing yet. */
	memset(batch, 0, sizeof(*batch));
	if (backend->devices_descriptor < 0)
		return ENOTSUP;

	/* Until a record asks for something, the cancel, or a failure. */
	for (;;) {
		/* The records, or the cancel. */
		entries[0].fd = backend->devices_descriptor;
		entries[0].events = POLLIN;
		entries[0].revents = 0;
		entries[1].fd = backend->cancel[0];
		entries[1].events = POLLIN;
		entries[1].revents = 0;
		count = 1;
		if (backend->cancel[0] >= 0)
			count = 2;
		ready = poll(entries, count, -1);
		if (ready < 0 && errno == EINTR)
			continue;
		if (ready < 0)
			return errno;

		/* The compositor stops. */
		if (count == 2 && entries[1].revents != 0)
			return ECANCELED;

		/* Every record waiting; a descriptor that fails ends the thread. */
		error = events_devices_read(backend, batch);
		if (error != 0)
			return error;

		/* Something to tell the event loop. */
		if (batch->input_changed || batch->keys_changed || batch->overflow || batch->count != 0U)
			break;
	}

	/* Succeeded: the batch holds what the records asked. */
	return 0;
}

/*
 * Acts on the notes of a batch on the event loop's thread: the refusals
 * are forgotten after an input device came or went, the keys are looked
 * at again, and lost records have every input device looked at again.
 */
void
kl_backend_devices_take(
	struct kl_backend *backend,
	const struct kl_backend_device_batch *batch)
{
	/* A device that came or went may be one that refused to open, which the next finding tries again. */
	if (batch->input_changed || batch->overflow)
		backend->input_refused_generation++;

	/* The security keys and the smart cards. */
	if (batch->keys_changed && backend->host.keys_changed != NULL)
		backend->host.keys_changed(backend->host.data);

	/* Lost records, or more devices than the batch held: a finding. */
	if (batch->overflow && backend->host.input_changed != NULL)
		backend->host.input_changed(backend->host.data);
}

/*
 * Closes the devices' descriptor after its thread ended, and gives its
 * classes back to the event loop's.
 */
void
kl_backend_devices_stop(
	struct kl_backend *backend)
{
	int error;

	/* Nothing started. */
	if (backend == NULL || backend->devices_descriptor < 0)
		return;

	/* The thread's descriptor. */
	(void)close(backend->devices_descriptor);
	backend->devices_descriptor = -1;

	/* The event loop hears the devices again. */
	if (backend->events_descriptor >= 0) {
		error = events_subscribe(backend->events_descriptor, EVENTS_CLASSES);
		if (error != 0)
			printf("KL DEVICES the event loop does not hear the devices errno=%d\n", error);
	}
}

/* Counts the events' descriptor: one while it is open. */
size_t
kl_backend_events_poll_count(
	const struct kl_backend *backend)
{
	/* Nothing open. */
	if (backend->events_descriptor < 0)
		return 0;

	/* Succeeded: the one descriptor. */
	return 1;
}

/* Fills the events' descriptor, polled for reading. */
void
kl_backend_events_poll_fill(
	struct kl_backend *backend,
	struct pollfd *descriptors)
{
	/* Nothing open. */
	if (backend->events_descriptor < 0)
		return;

	/* Readable when a record waits. */
	descriptors[0].fd = backend->events_descriptor;
	descriptors[0].events = POLLIN;
	descriptors[0].revents = 0;
}

/*
 * Reads every record waiting and tells the compositor: each button and lid
 * at once, the input devices and the power once after all of them.
 */
void
kl_backend_events_poll_done(
	struct kl_backend *backend,
	const struct pollfd *descriptors)
{
	struct system_event events[EVENTS_READ];
	unsigned asked;
	ssize_t length;
	size_t count;
	size_t index;

	/* Nothing open, or nothing to read. */
	if (backend->events_descriptor < 0)
		return;
	if ((descriptors[0].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) == 0)
		return;

	/* Every record waiting. */
	asked = 0U;
	for (;;) {
		/* A few records; none left ends the pass. */
		length = read(backend->events_descriptor, events, sizeof(events));
		if (length < 0 && (errno == EAGAIN || errno == EINTR))
			break;
		if (length < 0) {
			events_fail(backend, "read", errno);
			break;
		}

		/* The device's end (it does not end, but a descriptor that does is given up). */
		if (length == 0) {
			events_fail(backend, "end", 0);
			break;
		}

		/* Whole records only. */
		if ((size_t)length % sizeof(events[0]) != 0U) {
			events_fail(backend, "record", EPROTO);
			break;
		}

		/* Each record. */
		count = (size_t)length / sizeof(events[0]);
		for (index = 0; index < count; index++)
			asked |= events_dispatch(backend, &events[index]);
	}

	/*
	 * The input devices and the power, once each; a device that came or
	 * went may be one that refused to open, which the next scan tries again.
	 */
	if ((asked & EVENTS_INPUT) != 0U)
		backend->input_refused_generation++;
	if ((asked & EVENTS_INPUT) != 0U && backend->host.input_changed != NULL)
		backend->host.input_changed(backend->host.data);
	if ((asked & EVENTS_POWER) != 0U && backend->host.power_changed != NULL)
		backend->host.power_changed(backend->host.data);
	if ((asked & EVENTS_KEYS) != 0U && backend->host.keys_changed != NULL)
		backend->host.keys_changed(backend->host.data);
}

/*
 * Handles one record: a button or the lid is told at once; the input
 * devices and the power are returned as EVENTS_* bits, to be told after
 * every record of the pass.
 */
static unsigned
events_dispatch(
	struct kl_backend *backend,
	const struct system_event *event)
{
	const char *fido;
	unsigned button;
	unsigned open;
	int sleep_button;
	int release;
	int card;

	/* Each class. */
	switch (event->class_bit) {
	case KERN_SYSTEM_EVENT_INPUT:
		/* A FIDO node is a security key too. */
		fido = strstr(event->detail, EVENTS_FIDO_USAGE);
		if (fido != NULL)
			return EVENTS_INPUT | EVENTS_KEYS;
		return EVENTS_INPUT;
	case KERN_SYSTEM_EVENT_USB:
		/* Only a smart card slot or its card is the compositor's (a key held to an NFC reader). */
		card = strncmp(event->subject, EVENTS_CARD_SUBJECT, sizeof(EVENTS_CARD_SUBJECT) - 1U);
		if (card == 0)
			return EVENTS_KEYS;
		return 0U;
	case KERN_SYSTEM_EVENT_AC:
	case KERN_SYSTEM_EVENT_BATTERY:
		return EVENTS_POWER;
	case KERN_SYSTEM_EVENT_OVERFLOW:
		/* Records were lost: the devices, the power and the keys are looked at again. */
		return EVENTS_INPUT | EVENTS_POWER | EVENTS_KEYS;
	case KERN_SYSTEM_EVENT_POWER:
		/* A press of the power or the sleep button. */
		if (event->action != KERN_SYSTEM_EVENT_PRESS)
			return 0U;
		button = KL_BACKEND_BUTTON_POWER;
		sleep_button = strncmp(event->subject, "sleep-button", sizeof(event->subject));
		if (sleep_button == 0)
			button = KL_BACKEND_BUTTON_SLEEP;

		/* A power button's release is no press. */
		if (button == KL_BACKEND_BUTTON_POWER) {
			release = events_power_release(backend, event);
			if (release)
				return 0U;
		}

		/* The press, told to the compositor. */
		if (backend->host.power_button != NULL)
			backend->host.power_button(backend->host.data, button);
		return 0U;
	case KERN_SYSTEM_EVENT_LID:
		/* The lid opened or closed. */
		open = 0U;
		if (event->value != 0)
			open = 1U;
		if (backend->host.lid_changed != NULL)
			backend->host.lid_changed(backend->host.data, open);
		return 0U;
	default:
		/* A class not subscribed to. */
		return 0U;
	}
}

/*
 * Tells whether a power button's record is the release of the press
 * before it.  The Dell firmware of the Latitude 5320 and 5330 tells the
 * press and the release alike (the EC's query 0x66 runs Notify (PBTN,
 * 0x80) on both edges, WS182 p001), so the record that follows a press
 * within EVENTS_RELEASE_MS is taken as its release.  A record later than
 * that, or after a release, is a new press.  Returns 1 for a release.
 */
static int
events_power_release(
	struct kl_backend *backend,
	const struct system_event *event)
{
	uint64_t now_ms;
	uint64_t gap_ms;
	unsigned due;

	/* The record's time, and whether a press awaits its release (only the next record may be it). */
	now_ms = event->time_ns / 1000000U;
	due = backend->power_release_due;
	backend->power_release_due = 0U;

	/* Soon enough after the press: its release, dropped. */
	if (due != 0U && now_ms >= backend->power_press_ms) {
		gap_ms = now_ms - backend->power_press_ms;
		if (gap_ms < EVENTS_RELEASE_MS) {
			printf("KL EVENTS power-button release gap_ms=%llu\n", (unsigned long long)gap_ms);
			return 1;
		}
	}

	/* A new press: its release is awaited from now. */
	backend->power_release_due = 1U;
	backend->power_press_ms = now_ms;

	/* Succeeded: the record is a press. */
	return 0;
}

/* Gives up on the events after a failure, and says so in the log. */
static void
events_fail(
	struct kl_backend *backend,
	const char *what,
	int error)
{
	/* The log. */
	printf("KL EVENTS %s failed errno=%d; no more events\n", what, error);

	/* Nothing more is heard. */
	kl_backend_events_close(backend);
}

/* Sets the classes a descriptor of /dev/system hears: 0, or an errno value. */
static int
events_subscribe(
	int descriptor,
	uint32_t classes)
{
	struct system_event_subscription subscription;
	int result;

	/* The classes; an older kernel refuses them. */
	memset(&subscription, 0, sizeof(subscription));
	subscription.classes = classes;
	result = ioctl(descriptor, KERN_SYSTEM_EVENT_SUBSCRIBE, &subscription);
	if (result != 0)
		return errno;

	/* Succeeded: the descriptor hears them from now on. */
	return 0;
}

/* Reads every record waiting on the devices' descriptor into batch: 0, or an errno value. */
static int
events_devices_read(
	struct kl_backend *backend,
	struct kl_backend_device_batch *batch)
{
	struct system_event events[EVENTS_READ];
	ssize_t length;
	size_t count;
	size_t index;

	/* A few records at a time; none left ends the read. */
	for (;;) {
		length = read(backend->devices_descriptor, events, sizeof(events));
		if (length < 0 && errno == EINTR)
			continue;
		if (length < 0 && errno == EAGAIN)
			break;
		if (length < 0)
			return errno;

		/* The device does not end; a descriptor that does is given up, and so is a part of a record. */
		if (length == 0)
			return EIO;
		if ((size_t)length % sizeof(events[0]) != 0U)
			return EPROTO;

		/* Each record. */
		count = (size_t)length / sizeof(events[0]);
		for (index = 0; index < count; index++)
			events_device(backend, &events[index], batch);
	}

	/* Succeeded: every record waiting was read. */
	return 0;
}

/* Puts what one record of the devices asks into batch. */
static void
events_device(
	struct kl_backend *backend,
	const struct system_event *event,
	struct kl_backend_device_batch *batch)
{
	const char *fido;
	int card;

	/* Each class. */
	switch (event->class_bit) {
	case KERN_SYSTEM_EVENT_INPUT:
		/* A device came or went (a FIDO node is a security key too); an evdev node that came is opened. */
		batch->input_changed = 1U;
		fido = strstr(event->detail, EVENTS_FIDO_USAGE);
		if (fido != NULL)
			batch->keys_changed = 1U;
		if (event->action == KERN_SYSTEM_EVENT_ADD)
			events_input_came(backend, event->subject, batch);
		break;
	case KERN_SYSTEM_EVENT_USB:
		/* Only a smart card slot or its card is the compositor's (a key held to an NFC reader). */
		card = strncmp(event->subject, EVENTS_CARD_SUBJECT, sizeof(EVENTS_CARD_SUBJECT) - 1U);
		if (card == 0)
			batch->keys_changed = 1U;
		break;
	case KERN_SYSTEM_EVENT_OVERFLOW:
		/* Records were lost: every device and the keys are looked at again. */
		batch->input_changed = 1U;
		batch->keys_changed = 1U;
		batch->overflow = 1U;
		break;
	default:
		/* A class the thread does not hear. */
		break;
	}
}

/*
 * Opens the evdev node an INPUT record's subject names (eventN) into the
 * batch's next place; one beyond the batch's room is left to a finding.
 */
static void
events_input_came(
	struct kl_backend *backend,
	const char *subject,
	struct kl_backend_device_batch *batch)
{
	char path[KL_BACKEND_INPUT_PATH_MAX];
	size_t prefix;
	size_t length;
	size_t index;
	int same;
	int error;

	/* Only an evdev node: "event" and its number, all within the subject. */
	prefix = sizeof(EVENTS_EVDEV_PREFIX) - 1U;
	same = strncmp(subject, EVENTS_EVDEV_PREFIX, prefix);
	if (same != 0)
		return;
	length = strnlen(subject, KERN_SYSTEM_EVENT_SUBJECT_MAX);
	if (length == prefix || length >= KERN_SYSTEM_EVENT_SUBJECT_MAX)
		return;
	for (index = prefix; index < length; index++) {
		if (subject[index] < '0' || subject[index] > '9')
			return;
	}

	/* No room left: a finding looks for it. */
	if (batch->count >= KL_BACKEND_DEVICES_NODES_MAX) {
		batch->overflow = 1U;
		return;
	}

	/* The node, opened with its capabilities (through the seat socket when there is one). */
	(void)snprintf(path, sizeof(path), "%s/%.*s", EVENTS_INPUT_DIRECTORY, (int)length, subject);
	error = kl_backend_input_open(backend, path, &batch->nodes[batch->count]);
	printf("KL DEVICES input came node=%.*s error=%d\n", (int)length, subject, error);
	if (error != 0)
		return;

	/* Succeeded: the event loop offers it to the seat. */
	batch->count++;
}
