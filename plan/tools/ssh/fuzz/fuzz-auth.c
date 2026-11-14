/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* libFuzzer target: the service and user authentication messages, and the signed data of a request. */

#include <string.h>

#include "ssh-auth.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct ssh_auth_request request;
	struct ssh_buf out;
	char text[SSH_BANNER_MAX + 1];
	static const uint8_t session_id[SSH_SHA256_SIZE];
	uint8_t public_key[SSH_ED25519_PUBLIC];
	int partial;
	int error;

	/* Parses the service request, whatever the bytes are. */
	(void)ssh_auth_parse_service(data, size, text);

	/* Parses the bytes as a user authentication request. */
	error = ssh_auth_parse_request(data, size, &request);

	/* Exercises the signed-data builder and the clean-up only for a request that parsed. */
	if (error == 0) {
		if (request.method == SSH_AUTH_PUBLICKEY && request.has_signature) {
			ssh_buf_init(&out, SSH_PACKET_LIMIT + 64U, 0);
			ssh_auth_put_signed_data(session_id, data, request.signed_size, &out);
			ssh_buf_free(&out);
		}

		/* Releases what the parsed request holds. */
		ssh_auth_request_clear(&request);
	}

	/* Parses the bytes as the other server replies of the authentication protocol. */
	(void)ssh_auth_parse_failure(data, size, text, sizeof(text), &partial);
	memset(public_key, 1, sizeof(public_key));
	(void)ssh_auth_parse_pk_ok(data, size, public_key);
	(void)ssh_auth_parse_banner(data, size, text, sizeof(text));

	/* Succeeded: the input was handled without a crash. */
	return 0;
}
