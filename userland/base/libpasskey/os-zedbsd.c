/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libpasskey's operating system layer on zedBSD (ws161-p004; os.h,
 * docs/reference/security-keys.md): the raw HID nodes /dev/input/hidrawN.
 *
 * A node is a security key's when HIDRAW_GET_INFO says FIDO's usage page
 * and the CTAPHID usage.  The reports go through os-posix.c.  A key that
 * numbers its reports is not taken (FIDO keys do not).
 *
 * A smart card slot (ws161-p005) is /dev/smartcardN with a card in it
 * (CCID_GET_STATUS); opening it powers the card (CCID_POWER_ON, which
 * claims the slot for the open) and its APDUs go through CCID_TRANSMIT;
 * closing it powers the card off (CCID_POWER_OFF, and the close).
 */

#include "os.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <uapi/ccid.h>
#include <uapi/hidraw.h>

/* Where the nodes are, and the start of their names. */
#define OS_DIRECTORY	"/dev/input"
#define OS_PREFIX	"hidraw"

/* Where the smart card slots are, and the start of their names. */
#define OS_CARD_DIRECTORY	"/dev"
#define OS_CARD_PREFIX		"smartcard"

static int os_probe(const char *path, struct pk_os_device *device);
static int os_card_probe(const char *path, int any, struct pk_os_device *device);
static int os_card_list(int any, struct pk_os_device *devices, size_t capacity, size_t *count);
static int os_card_transmit(void *context, const uint8_t *command, size_t size, uint8_t *response, size_t capacity, size_t *response_size, unsigned timeout_ms);

/*
 * Lists the security keys: at most capacity of them in devices, their
 * number in *count.  Returns 0, or an errno value when the nodes cannot
 * be looked at.
 */
int
pk_os_list(
	struct pk_os_device *devices,
	size_t capacity,
	size_t *count)
{
	struct dirent *entry;
	DIR *directory;
	char path[PK_OS_PATH_MAX];
	int compared;
	int written;
	int error;

	/* None yet; the nodes' directory. */
	*count = 0U;
	directory = opendir(OS_DIRECTORY);
	if (directory == NULL)
		return errno;

	/* Each raw node that is a key's, while there is room. */
	for (;;) {
		entry = readdir(directory);
		if (entry == NULL || *count == capacity)
			break;
		compared = strncmp(entry->d_name, OS_PREFIX, sizeof(OS_PREFIX) - 1U);
		if (compared != 0)
			continue;
		written = snprintf(path, sizeof(path), "%s/%s", OS_DIRECTORY, entry->d_name);
		if (written < 0 || (size_t)written >= sizeof(path))
			continue;
		error = os_probe(path, &devices[*count]);
		if (error == 0)
			(*count)++;
	}

	/* Succeeded: the directory closed. */
	(void)closedir(directory);
	return 0;
}

/*
 * Opens a key's node, taking it for this open alone when grab is nonzero,
 * and gives its report functions in *io.  Returns 0, or an errno value.
 */
int
pk_os_open(
	struct pk_os_hid *handle,
	const char *path,
	int grab,
	struct pk_hid_io *io)
{
	struct hidraw_info info;
	int descriptor;
	int on;
	int result;
	int error;

	/* The node, read and written without waiting (poll() waits). */
	handle->descriptor = -1;
	descriptor = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (descriptor < 0)
		return errno;

	/* What it is. */
	result = ioctl(descriptor, HIDRAW_GET_INFO, &info);
	if (result < 0) {
		error = errno;
		(void)close(descriptor);
		return error;
	}

	/* A key's, with reports that are not numbered. */
	if (info.usage_page != HIDRAW_USAGE_PAGE_FIDO || (info.flags & HIDRAW_INFO_NUMBERED) != 0U) {
		(void)close(descriptor);
		return ENODEV;
	}

	/* Taken for this open alone, when asked. */
	if (grab) {
		on = 1;
		result = ioctl(descriptor, HIDRAW_GRAB, &on);
		if (result < 0) {
			error = errno;
			(void)close(descriptor);
			return error;
		}
	}

	/* Succeeded: the report functions on the node. */
	handle->descriptor = descriptor;
	pk_os_posix_io(handle, io);
	return 0;
}

/*
 * Tells whether a node is a key's, filling device when it is.  Returns 0,
 * ENODEV for another device's node, or an errno value.
 */
static int
os_probe(
	const char *path,
	struct pk_os_device *device)
{
	struct hidraw_info info;
	struct hidraw_text name;
	int descriptor;
	int result;
	int error;

	/* The node, only to ask it. */
	descriptor = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (descriptor < 0)
		return errno;

	/* What it is. */
	result = ioctl(descriptor, HIDRAW_GET_INFO, &info);
	if (result < 0) {
		error = errno;
		(void)close(descriptor);
		return error;
	}

	/* FIDO's CTAPHID. */
	if (info.usage_page != HIDRAW_USAGE_PAGE_FIDO || info.usage != HIDRAW_USAGE_CTAPHID) {
		(void)close(descriptor);
		return ENODEV;
	}

	/* Its name (none when the device gave none). */
	memset(&name, 0, sizeof(name));
	result = ioctl(descriptor, HIDRAW_GET_NAME, &name);
	if (result < 0)
		name.value[0] = '\0';
	(void)close(descriptor);

	/* Succeeded: the key. */
	(void)snprintf(device->path, sizeof(device->path), "%s", path);
	(void)snprintf(device->name, sizeof(device->name), "%.*s", (int)(sizeof(name.value) - 1U), name.value);
	device->vendor = info.vendor;
	device->product = info.product;
	return 0;
}

/*
 * Lists the smart card slots that hold a card: at most capacity of them in
 * devices, their number in *count.  Returns 0, or an errno value when the
 * nodes cannot be looked at.
 */
int
pk_os_list_cards(
	struct pk_os_device *devices,
	size_t capacity,
	size_t *count)
{
	/* The slots with a card. */
	return os_card_list(0, devices, capacity, count);
}

/*
 * Lists every smart card slot, with a card or without (ws199-p001: the
 * slots a key may be held to while something waits): at most capacity of
 * them in devices, their number in *count.  Returns 0, or an errno value
 * when the nodes cannot be looked at.
 */
int
pk_os_list_slots(
	struct pk_os_device *devices,
	size_t capacity,
	size_t *count)
{
	/* Every slot. */
	return os_card_list(1, devices, capacity, count);
}

/*
 * Attaches a smart card slot (ws199-p001): its node opened for its card's
 * events (without waiting: pk_os_card_event says EAGAIN) and for a later
 * pk_os_card_select, its card not powered and the slot not claimed.
 * Returns 0, or an errno value.
 */
int
pk_os_card_attach(
	struct pk_os_card *card,
	const char *path)
{
	int descriptor;

	/* The node. */
	card->descriptor = -1;
	descriptor = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (descriptor < 0)
		return errno;

	/* Succeeded: attached. */
	card->descriptor = descriptor;
	return 0;
}

/* Tells whether an attached slot holds a card now.  Returns 0, or an errno value. */
int
pk_os_card_present(
	struct pk_os_card *card,
	int *present)
{
	struct ccid_status status;
	int result;

	/* Its state. */
	*present = 0;
	result = ioctl(card->descriptor, CCID_GET_STATUS, &status);
	if (result < 0)
		return errno;

	/* Succeeded: a card unless absent. */
	*present = status.state != CCID_CARD_ABSENT;
	return 0;
}

/*
 * Reads one of an attached slot's events: *inserted 1 when a card came,
 * 0 when one went.  Returns 0, EAGAIN when none waits, ENODEV when the
 * reader is gone, or another errno value.
 */
int
pk_os_card_event(
	struct pk_os_card *card,
	int *inserted)
{
	struct ccid_event event;
	ssize_t got;

	/* One record, whole. */
	*inserted = 0;
	got = read(card->descriptor, &event, sizeof(event));
	if (got < 0)
		return errno;
	if (got == 0)
		return ENODEV;
	if ((size_t)got != sizeof(event))
		return EIO;

	/* Succeeded: what it says. */
	*inserted = event.kind == CCID_EVENT_INSERTED;
	return 0;
}

/*
 * Powers an attached slot's card (the slot claimed for this open) and
 * selects its FIDO applet, CTAP2 over it in transport.  A card that does
 * not answer (a reader's SAM slot, a card that is not a key, BUG-286) is
 * powered off again and the slot kept.  Returns 0, EBUSY while another
 * open holds the slot, or another errno value.
 */
int
pk_os_card_select(
	struct pk_os_card *card,
	struct pk_nfc *nfc,
	struct pk_transport *transport,
	unsigned timeout_ms)
{
	struct ccid_status status;
	struct ccid_info info;
	struct pk_nfc_io io;
	int result;
	int error;

	/* What the reader takes. */
	result = ioctl(card->descriptor, CCID_GET_INFO, &info);
	if (result < 0)
		return errno;

	/* The card powered (its ATR is not needed: the reader frames the card). */
	result = ioctl(card->descriptor, CCID_POWER_ON, &status);
	if (result < 0)
		return errno;

	/* The FIDO applet. */
	io.context = card;
	io.transmit = os_card_transmit;
	io.max_command = info.max_command;
	io.extended = (info.flags & CCID_INFO_EXTENDED_APDU) != 0U;
	error = pk_nfc_open(nfc, &io, timeout_ms);
	if (error != 0) {
		pk_os_card_power_off(card);
		return error;
	}

	/* Succeeded: CTAP2 over the applet. */
	(void)pk_nfc_transport(transport, nfc);
	return 0;
}

/* Powers an attached slot's card off and lets the slot go, the slot kept attached. */
void
pk_os_card_power_off(
	struct pk_os_card *card)
{
	/* Only an open slot. */
	if (card->descriptor < 0)
		return;

	/* Off. */
	(void)ioctl(card->descriptor, CCID_POWER_OFF);
}

/*
 * Opens a smart card slot and powers its card, the slot claimed for this
 * open, and gives its APDU function in *io.  Returns 0, or an errno value.
 */
int
pk_os_card_open(
	struct pk_os_card *card,
	const char *path,
	struct pk_nfc_io *io)
{
	struct ccid_status status;
	struct ccid_info info;
	int descriptor;
	int result;
	int error;

	/* The node. */
	card->descriptor = -1;
	descriptor = open(path, O_RDWR | O_CLOEXEC);
	if (descriptor < 0)
		return errno;

	/* What the reader takes. */
	result = ioctl(descriptor, CCID_GET_INFO, &info);
	if (result < 0) {
		error = errno;
		(void)close(descriptor);
		return error;
	}

	/* The card powered (its ATR is not needed: the reader frames the card). */
	result = ioctl(descriptor, CCID_POWER_ON, &status);
	if (result < 0) {
		error = errno;
		(void)close(descriptor);
		return error;
	}

	/* Succeeded: the slot's APDUs. */
	card->descriptor = descriptor;
	io->context = card;
	io->transmit = os_card_transmit;
	io->max_command = info.max_command;
	io->extended = (info.flags & CCID_INFO_EXTENDED_APDU) != 0U;
	return 0;
}

/* Powers a slot's card off and closes the slot (once). */
void
pk_os_card_close(
	struct pk_os_card *card)
{
	/* Once. */
	if (card->descriptor < 0)
		return;

	/* The card off (the last close would do it too), and the node. */
	(void)ioctl(card->descriptor, CCID_POWER_OFF);
	(void)close(card->descriptor);
	card->descriptor = -1;
}

/* Lists the smart card slots, every one (any) or those with a card.  Returns 0, or an errno value. */
static int
os_card_list(
	int any,
	struct pk_os_device *devices,
	size_t capacity,
	size_t *count)
{
	struct dirent *entry;
	DIR *directory;
	char path[PK_OS_PATH_MAX];
	int compared;
	int written;
	int error;

	/* None yet; the nodes' directory. */
	*count = 0U;
	directory = opendir(OS_CARD_DIRECTORY);
	if (directory == NULL)
		return errno;

	/* Each slot (with a card unless any), while there is room. */
	for (;;) {
		entry = readdir(directory);
		if (entry == NULL || *count == capacity)
			break;
		compared = strncmp(entry->d_name, OS_CARD_PREFIX, sizeof(OS_CARD_PREFIX) - 1U);
		if (compared != 0)
			continue;
		written = snprintf(path, sizeof(path), "%s/%s", OS_CARD_DIRECTORY, entry->d_name);
		if (written < 0 || (size_t)written >= sizeof(path))
			continue;
		error = os_card_probe(path, any, &devices[*count]);
		if (error == 0)
			(*count)++;
	}

	/* Succeeded: the directory closed. */
	(void)closedir(directory);
	return 0;
}

/*
 * Tells whether a node is a smart card slot (with a card in it unless
 * any), filling device when it is.  Returns 0, ENODEV for an empty slot
 * when a card is needed, or an errno value.
 */
static int
os_card_probe(
	const char *path,
	int any,
	struct pk_os_device *device)
{
	struct ccid_status status;
	struct ccid_info info;
	int descriptor;
	int result;
	int error;

	/* The node, only to ask it. */
	descriptor = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (descriptor < 0)
		return errno;

	/* What it is, and whether it holds a card. */
	result = ioctl(descriptor, CCID_GET_INFO, &info);
	if (result == 0)
		result = ioctl(descriptor, CCID_GET_STATUS, &status);
	if (result < 0) {
		error = errno;
		(void)close(descriptor);
		return error;
	}

	/* The node is not kept; an empty slot is no key's. */
	(void)close(descriptor);
	if (!any && status.state == CCID_CARD_ABSENT)
		return ENODEV;

	/* Succeeded: the slot. */
	(void)snprintf(device->path, sizeof(device->path), "%s", path);
	(void)snprintf(device->name, sizeof(device->name), "%.*s", (int)(sizeof(info.name) - 1U), info.name);
	device->vendor = info.vendor;
	device->product = info.product;
	return 0;
}

/* Exchanges one APDU through the slot. */
static int
os_card_transmit(
	void *context,
	const uint8_t *command,
	size_t size,
	uint8_t *response,
	size_t capacity,
	size_t *response_size,
	unsigned timeout_ms)
{
	struct ccid_transmit transmit;
	struct pk_os_card *card;
	int result;

	/* The exchange, its wait the caller's. */
	card = context;
	memset(&transmit, 0, sizeof(transmit));
	transmit.command = (uint64_t)(uintptr_t)command;
	transmit.response = (uint64_t)(uintptr_t)response;
	transmit.command_size = (uint32_t)size;
	transmit.response_capacity = (uint32_t)capacity;
	transmit.timeout_ms = timeout_ms;
	if (transmit.timeout_ms > CCID_TRANSMIT_MAX_MS)
		transmit.timeout_ms = CCID_TRANSMIT_MAX_MS;
	result = ioctl(card->descriptor, CCID_TRANSMIT, &transmit);
	if (result < 0)
		return errno;

	/* Succeeded: the answer's size. */
	*response_size = transmit.response_size;
	return 0;
}
