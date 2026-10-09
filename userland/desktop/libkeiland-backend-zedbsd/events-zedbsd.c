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
 */

#include "userland/desktop/libkeiland-backend/backend-private.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
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

/*
 * Opens /dev/system and subscribes to the compositor's classes.  A system
 * that does not have it, or a kernel that refuses the subscription, leaves
 * the descriptor -1.
 */
void
kl_backend_events_open(
	struct kl_backend *backend)
{
	struct system_event_subscription subscription;
	int descriptor;
	int result;

	/* The device, read without waiting and kept from the compositor's children. */
	descriptor = open(EVENTS_NODE, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (descriptor < 0) {
		printf("KL EVENTS unavailable errno=%d\n", errno);
		return;
	}

	/* The classes; an older kernel refuses them. */
	memset(&subscription, 0, sizeof(subscription));
	subscription.classes = EVENTS_CLASSES;
	result = ioctl(descriptor, KERN_SYSTEM_EVENT_SUBSCRIBE, &subscription);
	if (result != 0) {
		printf("KL EVENTS unavailable errno=%d\n", errno);
		(void)close(descriptor);
		return;
	}

	/* Succeeded: the events are heard from now on. */
	backend->events_descriptor = descriptor;
	printf("KL EVENTS subscribed classes=0x%x\n", (unsigned)EVENTS_CLASSES);
}

/* Closes the events' descriptor. */
void
kl_backend_events_close(
	struct kl_backend *backend)
{
	/* Nothing open. */
	if (backend->events_descriptor < 0)
		return;

	/* Closed; nothing more is heard. */
	(void)close(backend->events_descriptor);
	backend->events_descriptor = -1;
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

	/* The input devices and the power, once each. */
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
