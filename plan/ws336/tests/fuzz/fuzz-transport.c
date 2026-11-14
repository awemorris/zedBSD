/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libFuzzer target: one transport (the first byte picks the server or the
 * client side) fed the rest as received bytes in pieces, its messages
 * taken until it needs more or closes.  The server signs with a fixed
 * key (RFC 8032 7.1 TEST 1's seed); the client takes any host key.  The
 * seeds (fuzz-host.sh writes them) hold an identification line and a
 * KEXINIT, so the exchange is reached.
 */

#include <string.h>

#include "ssh-key.h"
#include "ssh-transport.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static struct ssh_key fuzz_host_key;
static int fuzz_ready;

static int
fuzz_host_blob(void *context, struct ssh_buf *out)
{
	(void)context;
	ssh_key_put_public_blob(fuzz_host_key.public_key, out);
	return ssh_buf_error(out);
}

static int
fuzz_sign(void *context, const uint8_t *h, struct ssh_buf *out)
{
	(void)context;
	return ssh_key_sign(&fuzz_host_key, h, SSH_KEX_HASH_SIZE, out);
}

static int
fuzz_check(void *context, const uint8_t *blob, size_t size)
{
	(void)context;
	(void)blob;
	(void)size;
	return 0;
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct ssh_transport transport;
	struct ssh_transport_ops ops;
	struct ssh_buf payload;
	const uint8_t *pending;
	size_t piece;
	size_t written;
	int status;
	int role;
	int error;
	enum ssh_role side;

	/* Derives the fixed host key from its seed the first time only. */
	if (!fuzz_ready) {
		static const uint8_t seed[32] = {
			0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60, 0xba, 0x84, 0x4a, 0xf4, 0x92, 0xec, 0x2c, 0xc4,
			0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32, 0x69, 0x19, 0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x7f, 0x60
		};
		memcpy(fuzz_host_key.seed, seed, sizeof(seed));
		(void)ssh_ed25519_public(fuzz_host_key.seed, fuzz_host_key.public_key);
		fuzz_host_key.private = 1;
		fuzz_ready = 1;
	}

	/* An empty input selects nothing to exercise. */
	if (size == 0)
		return 0;

	/* The first byte picks the server or the client side. */
	role = data[0] & 1;
	data++;
	size--;

	/* Gives the transport the three callbacks of the harness. */
	memset(&ops, 0, sizeof(ops));
	ops.host_key = fuzz_host_blob;
	ops.sign = fuzz_sign;
	ops.check_host_key = fuzz_check;

	/* Starts a transport on the chosen side. */
	side = SSH_ROLE_CLIENT;
	if (role != 0)
		side = SSH_ROLE_SERVER;
	error = ssh_transport_init(&transport, side, &ops, 100);
	if (error != 0)
		return 0;

	/* Prepares the buffer that receives each message the transport hands out. */
	ssh_buf_init(&payload, SSH_PACKET_LIMIT, 1);

	/* Feeds the input in pieces of at most 1500 bytes and takes every message that results. */
	while (size > 0) {
		piece = size;
		if (piece > 1500)
			piece = 1500;

		/* Gives the transport the piece. */
		status = ssh_transport_input(&transport, data, piece);
		data += piece;
		size -= piece;
		if (status != 0 && status != EMSGSIZE)
			break;

		/* Takes every message the transport has ready and answers each one that is not a transport message. */
		for (;;) {
			ssh_buf_reset(&payload);
			status = ssh_transport_next(&transport, &payload);
			if (status != 0)
				break;
			if (payload.size > 0 && payload.data[0] >= 50)
				(void)ssh_transport_send(&transport, payload.data, payload.size);
		}

		/* Takes the bytes the transport wants to send and tells it they were written. */
		pending = ssh_transport_pending(&transport, &written);
		(void)pending;
		ssh_transport_written(&transport, written);
		(void)ssh_transport_tick(&transport, 100 + SSH_TRANSPORT_REKEY_SECONDS);
		/* Stops when the transport has closed. */
		if (transport.state == SSH_TRANSPORT_CLOSED)
			break;
	}

	/* Releases the buffer and the transport. */
	ssh_buf_free(&payload);
	ssh_transport_free(&transport);

	/* Succeeded: the input was handled without a crash. */
	return 0;
}
