/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD lspci userland command.
 *
 * Lists the PCI functions the kernel found, one line each, in the numeric
 * form other systems print for lspci -n:
 *
 *	00:02.0 0300: 8086:46a8 (rev 0c)
 *
 * The functions are sorted by address; the kernel gives them in the order it
 * found them.  There is no table of vendor or device names; the numbers are
 * what the hardware says, and a name table would be large and carry its own
 * licence.
 *
 *	-D	always print the PCI segment (domain) in front of the bus
 *	-k	name the driver bound to each function
 *	-v	also print the subsystem identity and header type
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
static int compare_address(const void *left, const void *right);
static void print_function(const struct system_pci_device_info *info,
			   int domain, int driver, int verbose);

/*
 * Reports how the command is used.
 */
static void
usage(
	void)
{
	fprintf(stderr, "usage: lspci [-Dkv]\n");
}

/*
 * Orders two functions by segment, bus, device and function.
 */
static int
compare_address(
	const void *left,
	const void *right)
{
	const struct system_pci_device_info *a = left;
	const struct system_pci_device_info *b = right;
	uint64_t ka;
	uint64_t kb;

	/* Packs each address into one number and compares those. */
	ka = (uint64_t)a->segment << 24 | (uint64_t)a->bus << 16 |
	     (uint64_t)a->device << 8 | a->function;
	kb = (uint64_t)b->segment << 24 | (uint64_t)b->bus << 16 |
	     (uint64_t)b->device << 8 | b->function;
	if (ka != kb)
		return ka < kb ? -1 : 1;
	return 0;
}

/*
 * Prints one function.
 */
static void
print_function(
	const struct system_pci_device_info *info,
	int domain,
	int driver,
	int verbose)
{
	/* Prints the address, with the segment when asked or when it is not 0. */
	if (domain || info->segment != 0)
		printf("%04x:", info->segment);
	printf("%02x:%02x.%x %02x%02x: %04x:%04x", info->bus, info->device,
	       info->function, info->base_class, info->subclass, info->vendor,
	       info->product);

	/* Adds the revision and a programming interface that is not 0. */
	if (info->revision != 0)
		printf(" (rev %02x)", info->revision);
	if (info->programming_interface != 0)
		printf(" (prog-if %02x)", info->programming_interface);
	putchar('\n');

	/* Adds the subsystem and header type when asked. */
	if (verbose) {
		printf("\tSubsystem: %04x:%04x\n", info->subvendor,
		       info->subproduct);
		printf("\tHeader type: %02x\n", info->header_type);
	}

	/* Adds the bound driver when asked. */
	if (driver && info->driver[0] != '\0')
		printf("\tKernel driver in use: %.*s\n",
		       (int)sizeof(info->driver), info->driver);
}

/*
 * Runs the lspci command.
 */
int
main(
	int argc,
	char **argv)
{
	struct system_pci_device_info *functions = NULL;
	struct system_pci_device_info *grown;
	struct system_pci_device_info info;
	size_t count = 0;
	size_t capacity = 0;
	size_t position;
	uint32_t index;
	int descriptor;
	int domain = 0;
	int driver = 0;
	int verbose = 0;
	int option;

	/* Reads the options. */
	while ((option = getopt(argc, argv, "Dkv")) != -1) {
		switch (option) {
		case 'D':
			domain = 1;
			break;
		case 'k':
			driver = 1;
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
		fprintf(stderr, "lspci: /dev/system: %s\n", strerror(errno));
		return 1;
	}

	/* Asks for each function in turn until the kernel has no more. */
	for (index = 0;; index++) {
		memset(&info, 0, sizeof(info));
		info.index = index;
		if (ioctl(descriptor, KERN_SYSTEM_GET_PCI_DEVICE, &info) != 0) {
			if (errno == ENOENT)
				break;
			fprintf(stderr, "lspci: function %u: %s\n",
				(unsigned)index, strerror(errno));
			free(functions);
			close(descriptor);
			return 1;
		}

		/* Keeps it, growing the list when it is full. */
		if (count == capacity) {
			capacity = capacity == 0 ? 32 : capacity * 2;
			grown = realloc(functions, capacity * sizeof(*functions));
			if (grown == NULL) {
				fprintf(stderr, "lspci: %s\n", strerror(errno));
				free(functions);
				close(descriptor);
				return 1;
			}
			functions = grown;
		}
		functions[count++] = info;
	}

	/* Prints them in address order. */
	if (count != 0)
		qsort(functions, count, sizeof(*functions), compare_address);
	for (position = 0; position < count; position++)
		print_function(&functions[position], domain, driver, verbose);
	free(functions);

	/* Succeeded. */
	close(descriptor);
	return 0;
}
