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
	size_t pattern_size;
	int found;

	/* Fills in a key that no input line is expected to name. */
	memset(public_key, 3, sizeof(public_key));

	/* Looks the key up in the input as an authorized-keys file and as a known-hosts file. */
	(void)ssh_authorized_keys_find((const char *)data, size, public_key, &found, &skipped);
	(void)ssh_known_hosts_check((const char *)data, size, "host.example", 2222, public_key, &result, &skipped);

	/* Removes a host from the input as a known-hosts file. */
	ssh_buf_init(&out, SSH_HOSTFILE_MAX + 4096U, 0);
	(void)ssh_known_hosts_remove((const char *)data, size, "host.example", 22, &out, &removed);
	ssh_buf_free(&out);

	/* Matches a host name against the input as a pattern list, limited to the length a pattern list may have. */
	pattern_size = size;
	if (pattern_size > 512)
		pattern_size = 512;
	(void)ssh_pattern_list_match((const char *)data, pattern_size, "Host.Example.org", 1);

	/* Succeeded: the input was handled without a crash. */
	return 0;
}
