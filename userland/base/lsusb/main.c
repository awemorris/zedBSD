/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD lsusb userland command.
 *
 * Lists the USB devices the kernel knows, one line each, sorted by bus and
 * address, in the form other systems print:
 *
 *	Bus 001 Device 002: ID 0627:0001
 *
 * There is no table of vendor or product names; the numbers are what the
 * device says.  Root hubs are listed with device 000.
 *
 *	-t	also print where each device is plugged in (the port on each hub
 *		from the root down), its class, speed and bound drivers
 *	-v	also print the descriptor fields the kernel reports
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <uapi/system.h>

static void usage(void);
static int compare_device(const void *left, const void *right);
static const char *speed_name(unsigned speed);
static void print_device(const struct system_usb_device_info *info,
			 int tree, int verbose);

/*
 * Reports how the command is used.
 */
static void
usage(
	void)
{
	fprintf(stderr, "usage: lsusb [-tv]\n");
}

/*
 * Orders two devices by bus and address.
 */
static int
compare_device(
	const void *left,
	const void *right)
{
	const struct system_usb_device_info *a = left;
	const struct system_usb_device_info *b = right;

	/* Compares the bus, then the address. */
	if (a->bus != b->bus)
		return a->bus < b->bus ? -1 : 1;
	if (a->address != b->address)
		return a->address < b->address ? -1 : 1;
	return 0;
}

/*
 * Names a speed the way USB specifications do, in bits per second.
 */
static const char *
speed_name(
	unsigned speed)
{
	/* Chooses the name. */
	switch (speed) {
	case KERN_SYSTEM_USB_SPEED_LOW:
		return "1.5M";
	case KERN_SYSTEM_USB_SPEED_FULL:
		return "12M";
	case KERN_SYSTEM_USB_SPEED_HIGH:
		return "480M";
	case KERN_SYSTEM_USB_SPEED_SUPER:
		return "5000M";
	case KERN_SYSTEM_USB_SPEED_SUPER_PLUS:
		return "10000M";
	default:
		return "unknown";
	}
}

/*
 * Prints one device.
 */
static void
print_device(
	const struct system_usb_device_info *info,
	int tree,
	int verbose)
{
	unsigned n;

	/* Prints the summary line. */
	printf("Bus %03u Device %03u: ID %04x:%04x%s\n", info->bus,
	       info->address, info->vendor, info->product,
	       (info->flags & KERN_SYSTEM_USB_ROOT_HUB) != 0 ? " root hub" : "");

	/* Adds the place, class, speed and drivers when asked. */
	if (tree) {
		printf("\tPort path: %u", info->bus);
		for (n = 0; n < info->port_depth &&
			    n < KERN_SYSTEM_USB_PORT_DEPTH_MAX; n++)
			printf("-%u", info->port_path[n]);
		printf("  Class %02x  Speed %s", info->device_class,
		       speed_name(info->speed));
		if (info->driver[0] != '\0')
			printf("  Driver %.*s", (int)sizeof(info->driver),
			       info->driver);
		putchar('\n');
	}

	/* Adds the descriptor fields when asked. */
	if (verbose) {
		printf("\tbcdUSB %x.%02x  bcdDevice %x.%02x\n",
		       info->usb_version >> 8, info->usb_version & 0xffU,
		       info->device_version >> 8, info->device_version & 0xffU);
		printf("\tbDeviceClass %02x  bDeviceSubClass %02x  "
		       "bDeviceProtocol %02x\n", info->device_class,
		       info->device_subclass, info->device_protocol);
		printf("\tbNumConfigurations %u  interfaces %u\n",
		       info->configuration_count, info->interface_count);
	}
}

/*
 * Runs the lsusb command.
 */
int
main(
	int argc,
	char **argv)
{
	struct system_usb_device_info *devices = NULL;
	struct system_usb_device_info *grown;
	struct system_usb_device_info info;
	size_t count = 0;
	size_t capacity = 0;
	size_t position;
	uint32_t index;
	int descriptor;
	int tree = 0;
	int verbose = 0;
	int option;

	/* Reads the options. */
	while ((option = getopt(argc, argv, "tv")) != -1) {
		switch (option) {
		case 't':
			tree = 1;
			break;
		case 'v':
			verbose = 1;
			break;
		default:
			usage();
			return 2;
		}
	}
	if (optind != argc) {
		usage();
		return 2;
	}

	/* Opens the system device. */
	descriptor = open("/dev/system", O_RDONLY);
	if (descriptor < 0) {
		fprintf(stderr, "lsusb: /dev/system: %s\n", strerror(errno));
		return 1;
	}

	/* Asks for each device in turn until the kernel has no more. */
	for (index = 0;; index++) {
		memset(&info, 0, sizeof(info));
		info.index = index;
		if (ioctl(descriptor, KERN_SYSTEM_GET_USB_DEVICE, &info) != 0) {
			if (errno == ENOENT)
				break;
			fprintf(stderr, "lsusb: device %u: %s\n",
				(unsigned)index, strerror(errno));
			free(devices);
			close(descriptor);
			return 1;
		}

		/* Keeps it, growing the list when it is full. */
		if (count == capacity) {
			capacity = capacity == 0 ? 16 : capacity * 2;
			grown = realloc(devices, capacity * sizeof(*devices));
			if (grown == NULL) {
				fprintf(stderr, "lsusb: %s\n", strerror(errno));
				free(devices);
				close(descriptor);
				return 1;
			}
			devices = grown;
		}
		devices[count++] = info;
	}

	/* Prints them in bus and address order. */
	if (count != 0)
		qsort(devices, count, sizeof(*devices), compare_device);
	for (position = 0; position < count; position++)
		print_device(&devices[position], tree, verbose);
	free(devices);

	/* Succeeded. */
	close(descriptor);
	return 0;
}
