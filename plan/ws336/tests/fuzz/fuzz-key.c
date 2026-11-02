/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* libFuzzer target: the public key blob, the one-line form, the private key file, a signature blob. */

#include <string.h>

#include "ssh-key.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct ssh_key key;
	uint8_t public_key[SSH_ED25519_PUBLIC];
	static const uint8_t message[] = "message";

	if (size == 0)
		return 0;
	switch (data[0] % 4) {
	case 0:
		(void)ssh_key_parse_public_blob(data + 1, size - 1, public_key);
		break;
	case 1:
		(void)ssh_key_parse_public_line((const char *)data + 1, size - 1, &key);
		ssh_key_clear(&key);
		break;
	case 2:
		(void)ssh_key_parse_private((const char *)data + 1, size - 1, &key);
		ssh_key_clear(&key);
		break;
	default:
		memset(public_key, 7, sizeof(public_key));
		(void)ssh_key_verify(public_key, message, sizeof(message), data + 1, size - 1);
		break;
	}
	return 0;
}
