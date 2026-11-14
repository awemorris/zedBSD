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

	if (size == 0)
		return 0;
	if (data[0] & 1) {
		sshd_config_defaults(&server);
		if (sshd_config_parse(&server, (const char *)data + 1, size - 1, error) == 0)
			(void)sshd_config_finish(&server);
	} else {
		ssh_config_init(&client);
		(void)ssh_config_parse(&client, (const char *)data + 1, size - 1, "host.example", error);
		option = malloc(size);
		if (option != NULL) {
			memcpy(option, data + 1, size - 1);
			option[size - 1] = '\0';
			(void)ssh_config_option(&client, option, error);
			free(option);
		}

		ssh_config_defaults(&client);
	}

	return 0;
}
