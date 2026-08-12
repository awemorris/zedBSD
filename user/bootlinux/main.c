/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <zedbsd/boot.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	struct zedbsd_boot_linux request;
	char arguments[4096];
	int at = 1, kernel_fd, boot_fd, i;

	memset(&request, 0, sizeof(request));
	request.boot_device_index = -1;
	if (at + 1 < argc && !strcmp(argv[at], "-d")) {
		char *end;
		long value = strtol(argv[at + 1], &end, 10);
		if (*end != '\0' || value < 0 || value > INT32_MAX) {
			fprintf(stderr, "linux: invalid device\n");
			return 2;
		}
		request.boot_device_index = (int32_t)value;
		at += 2;
	}
	if (at >= argc) {
		fprintf(stderr, "usage: linux [-d index] kernel [arguments...]\n");
		return 2;
	}
	kernel_fd = open(argv[at++], O_RDONLY);
	if (kernel_fd < 0) {
		fprintf(stderr, "linux: kernel: %d\n", errno);
		return 1;
	}
	arguments[0] = '\0';
	for (i = at; i < argc; i++) {
		size_t used = strlen(arguments), length = strlen(argv[i]);
		if (length + (used != 0) + 1U > sizeof(arguments) - used) {
			fprintf(stderr, "linux: command line too long\n");
			close(kernel_fd);
			return 2;
		}
		if (used != 0) arguments[used++] = ' ';
		memcpy(arguments + used, argv[i], length + 1U);
	}
	boot_fd = open("/dev/boot", O_RDONLY);
	if (boot_fd < 0) {
		fprintf(stderr, "linux: /dev/boot: %d\n", errno);
		close(kernel_fd);
		return 1;
	}
	request.kernel_fd = kernel_fd;
	request.command_line = (uint32_t)(uintptr_t)arguments;
	request.command_line_length = (uint32_t)strlen(arguments);
	if (ioctl(boot_fd, ZEDBSD_BOOT_LINUX, &request) != 0) {
		fprintf(stderr, "linux: ioctl: %d\n", errno);
		close(boot_fd);
		close(kernel_fd);
		return 1;
	}
	return 0;
}
