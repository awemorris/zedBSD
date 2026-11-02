/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* libFuzzer target: authorized_keys and known_hosts text, removal, and host pattern lists. */

#include <string.h>

#include "ssh-hostfile.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct ssh_buf out;
	enum ssh_known_result result;
	uint8_t public_key[SSH_ED25519_PUBLIC];
	unsigned skipped;
	unsigned removed;
	int found;

	memset(public_key, 3, sizeof(public_key));
	(void)ssh_authorized_keys_find((const char *)data, size, public_key, &found, &skipped);
	(void)ssh_known_hosts_check((const char *)data, size, "host.example", 2222, public_key, &result, &skipped);
	ssh_buf_init(&out, SSH_HOSTFILE_MAX + 4096U, 0);
	(void)ssh_known_hosts_remove((const char *)data, size, "host.example", 22, &out, &removed);
	ssh_buf_free(&out);
	(void)ssh_pattern_list_match((const char *)data, size > 512 ? 512 : size, "Host.Example.org", 1);
	return 0;
}
