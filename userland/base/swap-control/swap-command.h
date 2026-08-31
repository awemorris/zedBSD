/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares the zedBSD userland swap command interface.
 */

#ifndef ZEDBSD_USERLAND_SWAP_COMMAND_H
#define ZEDBSD_USERLAND_SWAP_COMMAND_H

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <zedbsd/system.h>

#ifndef SWAP_COMMAND_OPEN
#define SWAP_COMMAND_OPEN open
#endif
#ifndef SWAP_COMMAND_IOCTL
#define SWAP_COMMAND_IOCTL ioctl
#endif
#ifndef SWAP_COMMAND_CLOSE
#define SWAP_COMMAND_CLOSE close
#endif

static void
swap_command_usage(const char *program)
{
	fprintf(stderr, "usage: %s [--] SOURCE...\n", program);
}

static int
swap_command_is_operand(int *options, const char *argument)
{
	if (*options && strcmp(argument, "--") == 0) {
		*options = 0;
		return 0;
	}
	return 1;
}

static void
swap_command_report_error(const char *program, const char *source, int error)
{
	fprintf(stderr, "%s: %s: %s\n", program, source, strerror(error));
}

static int
swap_command_make_request(struct system_swap_control *control,
			  const char *source)
{
	size_t length;

	for (length = 0; length < sizeof(control->source); length++) {
		if (source[length] == '\0')
			break;
	}
	if (length == sizeof(control->source)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	memset(control, 0, sizeof(*control));
	control->version = ZEDBSD_SYSTEM_SWAP_VERSION;
	control->struct_size = (uint32_t)sizeof(*control);
	memcpy(control->source, source, length + 1);
	return 0;
}

static int
swap_command_main(const char *program, unsigned long request, int argc,
		  char **argv)
{
	int descriptor, error, index, operands = 0, options = 1, status = 0;

	for (index = 1; index < argc; index++) {
		if (!swap_command_is_operand(&options, argv[index]))
			continue;
		if (options && argv[index][0] == '-') {
			swap_command_usage(program);
			return 2;
		}
		operands++;
	}
	if (operands == 0) {
		swap_command_usage(program);
		return 2;
	}

	descriptor = SWAP_COMMAND_OPEN("/dev/system", O_RDWR);
	if (descriptor < 0) {
		error = errno;
		options = 1;
		for (index = 1; index < argc; index++) {
			if (swap_command_is_operand(&options, argv[index]))
				swap_command_report_error(program, argv[index], error);
		}
		return 1;
	}

	options = 1;
	for (index = 1; index < argc; index++) {
		struct system_swap_control control;

		if (!swap_command_is_operand(&options, argv[index]))
			continue;
		if (swap_command_make_request(&control, argv[index]) != 0 ||
		    SWAP_COMMAND_IOCTL(descriptor, request, &control) != 0) {
			error = errno;
			swap_command_report_error(program, argv[index], error);
			status = 1;
		}
	}
	(void)SWAP_COMMAND_CLOSE(descriptor);
	return status;
}

#endif
