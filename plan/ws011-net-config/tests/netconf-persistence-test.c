/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/net/netconf.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void
require(int condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "NPER: %s\n", message);
		exit(1);
	}
}

int
main(int argc, char **argv)
{
	struct netconf original, loaded, invalid;
	struct stat lock_status;
	char error[160] = "", temporary[512];
	pid_t child;
	int lock_descriptor, status;

	require(argc == 3, "usage: test DEFAULT OUTPUT");
	require(netconf_load(argv[1], &original, error, sizeof(error)) == 0,
		"default configuration does not load");
	require(netconf_save_atomic(argv[2], &original, error, sizeof(error)) ==
		    0,
		"atomic save failed");
	require(netconf_load(argv[2], &loaded, error, sizeof(error)) == 0,
		"saved configuration does not load");

	lock_descriptor = netconf_writer_lock(error, sizeof(error));
	require(lock_descriptor >= 0, "cannot acquire writer lock");
	require(fstat(lock_descriptor, &lock_status) == 0 &&
		    S_ISREG(lock_status.st_mode) &&
		    (lock_status.st_mode & 07777U) == 0600U,
		"writer lock is not a mode-0600 regular file");
	child = fork();
	require(child >= 0, "cannot fork lock contender");
	if (child == 0) {
		int contender = netconf_writer_lock(error, sizeof(error));
		if (contender >= 0) {
			(void)netconf_writer_unlock(contender);
			_exit(1);
		}
		_exit(errno == EACCES || errno == EAGAIN ? 0 : 1);
	}
	require(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
		    WEXITSTATUS(status) == 0,
		"concurrent writer was not rejected");
	require(netconf_writer_unlock(lock_descriptor) == 0,
		"cannot release writer lock");
	require(loaded.interface_count == 1 &&
		    strcmp(loaded.interfaces[0].name, "lo0") == 0 &&
		    loaded.interfaces[0].addresses[0].prefix_length == 8,
		"saved configuration changed the loopback model");

	invalid = original;
	invalid.version = 0;
	errno = 0;
	require(netconf_save_atomic(argv[2], &invalid, error, sizeof(error)) !=
			0 &&
		    errno == EINVAL,
		"invalid candidate was saved");
	require(netconf_load(argv[2], &loaded, error, sizeof(error)) == 0 &&
		    loaded.version == 1,
		"failed save damaged the prior file");

	require(snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", argv[2],
			 (long)getpid()) < (int)sizeof(temporary),
		"temporary path overflow");
	{
		FILE *stream = fopen(temporary, "w");
		require(stream != NULL && fclose(stream) == 0,
			"cannot create interrupted-save fixture");
	}
	require(netconf_save_atomic(argv[2], &original, error, sizeof(error)) !=
		    0,
		"save ignored an existing same-operation temporary");
	require(access(temporary, F_OK) == 0,
		"save removed a temporary file it did not create");
	require(netconf_load(argv[2], &loaded, error, sizeof(error)) == 0,
		"temporary collision damaged the prior file");
	(void)unlink(temporary);
	(void)unlink(argv[2]);
	puts("WS011 net.conf persistence: PASS");
	return 0;
}
