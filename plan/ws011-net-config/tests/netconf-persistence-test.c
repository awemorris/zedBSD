/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/net/netconf.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
	char error[160] = "", temporary[512];

	require(argc == 3, "usage: test DEFAULT OUTPUT");
	require(netconf_load(argv[1], &original, error, sizeof(error)) == 0,
		"default configuration does not load");
	require(netconf_save_atomic(argv[2], &original, error, sizeof(error)) ==
		    0,
		"atomic save failed");
	require(netconf_load(argv[2], &loaded, error, sizeof(error)) == 0,
		"saved configuration does not load");
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
