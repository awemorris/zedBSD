/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/command.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#include <unistd.h>
#define DMESG_LIMIT (1024U * 1024U)
int
main(int argc, char **argv)
{
	char *buffer;
	size_t size = 0, capacity;
	int tries = 0;
	(void)argv;
	if (argc != 1) {
		fprintf(stderr, "usage: dmesg\n");
		return 2;
	}
	if (sysctlbyname("kern.msgbuf", NULL, &size, NULL, 0)) {
		command_error("dmesg", NULL);
		return 1;
	}
	if (size > DMESG_LIMIT) {
		errno = EOVERFLOW;
		command_error("dmesg", NULL);
		return 1;
	}
	do {
		capacity = size;
		buffer = malloc(capacity ? capacity : 1U);
		if (!buffer) {
			command_error("dmesg", NULL);
			return 1;
		}
		size = capacity;
		if (sysctlbyname("kern.msgbuf", buffer, &size, NULL, 0) == 0)
			break;
		free(buffer);
		if (errno != ENOMEM || ++tries > 1) {
			command_error("dmesg", NULL);
			return 1;
		}
		if (sysctlbyname("kern.msgbuf", NULL, &size, NULL, 0)) {
			command_error("dmesg", NULL);
			return 1;
		}
		if (size > DMESG_LIMIT) {
			errno = EOVERFLOW;
			command_error("dmesg", NULL);
			return 1;
		}
	} while (1);
	if (size && command_write_all(STDOUT_FILENO, buffer, size)) {
		command_error("dmesg", NULL);
		free(buffer);
		return 1;
	}
	if (size && buffer[size - 1] != '\n' &&
	    command_write_all(STDOUT_FILENO, "\n", 1)) {
		command_error("dmesg", NULL);
		free(buffer);
		return 1;
	}
	free(buffer);
	return 0;
}
