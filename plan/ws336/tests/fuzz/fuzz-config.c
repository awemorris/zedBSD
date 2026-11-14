/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* libFuzzer target: sshd_config and ssh_config text, and a -o option. */

#include <stdlib.h>
#include <string.h>

#include "ssh-config.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct sshd_config server;
	struct ssh_config client;
	char error[SSH_CONFIG_ERROR_MAX];
	char *option;
	int status;

	/* An empty input selects nothing to exercise. */
	if (size == 0)
		return 0;

	/* The low bit of the first byte picks the server or the client configuration. */
	if (data[0] & 1) {
		/* Parses the rest as a server configuration and finishes it when it parsed. */
		sshd_config_defaults(&server);
		status = sshd_config_parse(&server, (const char *)data + 1, size - 1, error);
		if (status == 0)
			(void)sshd_config_finish(&server);
	} else {
		/* Parses the rest as a client configuration for one host. */
		ssh_config_init(&client);
		(void)ssh_config_parse(&client, (const char *)data + 1, size - 1, "host.example", error);
		/* Copies the rest into a heap string so that a read past its end is caught, and parses it as one option. */
		option = malloc(size);
		if (option != NULL) {
			memcpy(option, data + 1, size - 1);
			option[size - 1] = '\0';
			(void)ssh_config_option(&client, option, error);
			free(option);
		}

		/* Resets the client configuration to its defaults. */
		ssh_config_defaults(&client);
	}

	/* Succeeded: the input was handled without a crash. */
	return 0;
}
