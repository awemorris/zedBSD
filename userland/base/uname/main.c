/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Returns system name (POSIX XCU uname).
 *
 *	uname [-amnrsv]
 *
 * The fields asked for are written in the order system name, node name,
 * release, version and machine, separated by spaces; -a asks for all of
 * them and no option for the system name alone.  The PC-98 build adds
 * "pc98" after the version, which tells it from a PC/AT on the same CPU.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

/* The fields uname writes, one bit each, in their order. */
#define UNAME_SYSTEM 0x01
#define UNAME_NODE 0x02
#define UNAME_RELEASE 0x04
#define UNAME_VERSION 0x08
#define UNAME_MACHINE 0x10
#define UNAME_ALL 0x1f

static void write_field(int *written, const char *text);
static void usage(void);

/*
 * Runs uname.
 */
int
main(
	int argc,
	char **argv)
{
	struct utsname names;
	int fields;
	int option;
	int written;
	int status;

	/* Reads the options; uname takes no operand. */
	fields = 0;
	for (;;) {
		option = getopt(argc, argv, "amnrsv");
		if (option == -1)
			break;

		/* Adds the field the option names. */
		switch (option) {
		case 'a':
			fields |= UNAME_ALL;
			break;
		case 's':
			fields |= UNAME_SYSTEM;
			break;
		case 'n':
			fields |= UNAME_NODE;
			break;
		case 'r':
			fields |= UNAME_RELEASE;
			break;
		case 'v':
			fields |= UNAME_VERSION;
			break;
		case 'm':
			fields |= UNAME_MACHINE;
			break;
		default:
			usage();
			break;
		}
	}

	/* An operand is an error. */
	if (optind < argc)
		usage();

	/* No option is the system name. */
	if (fields == 0)
		fields = UNAME_SYSTEM;

	/* Asks the system for its names. */
	status = uname(&names);
	if (status != 0) {
		fprintf(stderr, "uname: %s\n", strerror(errno));
		return 1;
	}

	/* Writes the fields asked for, in order. */
	written = 0;
	if (fields & UNAME_SYSTEM)
		write_field(&written, names.sysname);
	if (fields & UNAME_NODE)
		write_field(&written, names.nodename);
	if (fields & UNAME_RELEASE)
		write_field(&written, names.release);
	if (fields & UNAME_VERSION) {
		write_field(&written, names.version);
#ifdef KERN_UNAME_PC98
		write_field(&written, "pc98");
#endif
	}

	/* The machine comes last. */
	if (fields & UNAME_MACHINE)
		write_field(&written, names.machine);
	putchar('\n');

	/* A failed write is an error. */
	fflush(stdout);
	status = ferror(stdout);
	if (status != 0) {
		fprintf(stderr, "uname: write error\n");
		return 1;
	}

	/* Succeeded: the names were written. */
	return 0;
}

/* Writes one field, after a space unless it is the first. */
static void
write_field(
	int *written,
	const char *text)
{
	/* A space separates the fields. */
	if (*written)
		putchar(' ');
	fputs(text, stdout);
	*written = 1;
}

/* Writes the usage message and exits with an error status. */
static void
usage(void)
{
	/* Names the POSIX form. */
	fprintf(stderr, "usage: uname [-amnrsv]\n");
	exit(1);
}
