/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libpasskey's operating system layer on Linux (ws161-p004; os.h): the raw
 * HID nodes /dev/hidrawN.
 *
 * A node is a security key's when its report descriptor's first
 * application collection is FIDO's CTAPHID (descriptor.c).  Linux has no
 * grab: the key is shared with the other programs that opened it, which
 * CTAPHID's channels keep apart.  The reports go through os-posix.c.
 */

#include "os.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* Where the nodes are, and the start of their names. */
#define OS_DIRECTORY	"/dev"
#define OS_PREFIX	"hidraw"

static int os_probe(const char *path, struct pk_os_device *device);
static int os_is_key(int descriptor);

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
 * Opens a key's node and gives its report functions in *io (grab is not
 * possible on Linux and is ignored).  Returns 0, or an errno value.
 */
int
pk_os_open(
	struct pk_os_hid *handle,
	const char *path,
	int grab,
	struct pk_hid_io *io)
{
	int descriptor;
	int key;

	/* The node, read and written without waiting (poll() waits). */
	(void)grab;
	handle->descriptor = -1;
	descriptor = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (descriptor < 0)
		return errno;

	/* A key's. */
	key = os_is_key(descriptor);
	if (!key) {
		(void)close(descriptor);
		return ENODEV;
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
	struct hidraw_devinfo info;
	char name[PK_OS_NAME_MAX];
	int descriptor;
	int result;
	int key;
	int error;

	/* The node, only to ask it. */
	descriptor = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (descriptor < 0)
		return errno;

	/* A key's. */
	key = os_is_key(descriptor);
	if (!key) {
		(void)close(descriptor);
		return ENODEV;
	}

	/* Its vendor and product. */
	result = ioctl(descriptor, HIDIOCGRAWINFO, &info);
	if (result < 0) {
		error = errno;
		(void)close(descriptor);
		return error;
	}

	/* Its name (none when the device gave none). */
	memset(name, 0, sizeof(name));
	result = ioctl(descriptor, HIDIOCGRAWNAME(sizeof(name) - 1U), name);
	if (result < 0)
		name[0] = '\0';
	(void)close(descriptor);

	/* Succeeded: the key. */
	(void)snprintf(device->path, sizeof(device->path), "%s", path);
	(void)snprintf(device->name, sizeof(device->name), "%s", name);
	device->vendor = (uint16_t)info.vendor;
	device->product = (uint16_t)info.product;
	return 0;
}

/* Tells whether an open node's report descriptor is a key's: 1 or 0. */
static int
os_is_key(
	int descriptor)
{
	struct hidraw_report_descriptor report;
	int size;
	int result;
	int key;

	/* The descriptor's size, then the descriptor. */
	result = ioctl(descriptor, HIDIOCGRDESCSIZE, &size);
	if (result < 0 || size <= 0 || size > HID_MAX_DESCRIPTOR_SIZE)
		return 0;
	memset(&report, 0, sizeof(report));
	report.size = (uint32_t)size;
	result = ioctl(descriptor, HIDIOCGRDESC, &report);
	if (result < 0)
		return 0;

	/* FIDO's first application collection. */
	key = pk_os_descriptor_is_fido(report.value, report.size);
	return key;
}

/* Lists no smart card slot: NFC on Linux is later (plan/ws161/phase001 section 9.4). */
int
pk_os_list_cards(
	struct pk_os_device *devices,
	size_t capacity,
	size_t *count)
{
	/* None. */
	(void)devices;
	(void)capacity;
	*count = 0U;
	return 0;
}

/* Opens no smart card slot (none is listed). */
int
pk_os_card_open(
	struct pk_os_card *card,
	const char *path,
	struct pk_nfc_io *io)
{
	/* Not on this system. */
	(void)path;
	(void)io;
	card->descriptor = -1;
	return ENOTSUP;
}

/* Closes no smart card slot. */
void
pk_os_card_close(
	struct pk_os_card *card)
{
	/* Nothing open. */
	card->descriptor = -1;
}

/* Lists no smart card slot (ws199-p001). */
int
pk_os_list_slots(
	struct pk_os_device *devices,
	size_t capacity,
	size_t *count)
{
	/* None. */
	(void)devices;
	(void)capacity;
	*count = 0U;
	return 0;
}

/* Attaches no smart card slot. */
int
pk_os_card_attach(
	struct pk_os_card *card,
	const char *path)
{
	/* Not on this system. */
	(void)path;
	card->descriptor = -1;
	return ENOTSUP;
}

/* Tells of no card. */
int
pk_os_card_present(
	struct pk_os_card *card,
	int *present)
{
	/* Not on this system. */
	(void)card;
	*present = 0;
	return ENOTSUP;
}

/* Reads no card's event. */
int
pk_os_card_event(
	struct pk_os_card *card,
	int *inserted)
{
	/* Not on this system. */
	(void)card;
	*inserted = 0;
	return ENOTSUP;
}

/* Selects no card's applet. */
int
pk_os_card_select(
	struct pk_os_card *card,
	struct pk_nfc *nfc,
	struct pk_transport *transport,
	unsigned timeout_ms)
{
	/* Not on this system. */
	(void)card;
	(void)nfc;
	(void)transport;
	(void)timeout_ms;
	return ENOTSUP;
}

/* Powers no card off. */
void
pk_os_card_power_off(
	struct pk_os_card *card)
{
	/* Nothing open. */
	(void)card;
}
