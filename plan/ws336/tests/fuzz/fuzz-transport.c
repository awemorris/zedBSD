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

	if (size == 0)
		return 0;
	role = data[0] & 1;
	data++;
	size--;
	memset(&ops, 0, sizeof(ops));
	ops.host_key = fuzz_host_blob;
	ops.sign = fuzz_sign;
	ops.check_host_key = fuzz_check;
	if (ssh_transport_init(&transport, role ? SSH_ROLE_SERVER : SSH_ROLE_CLIENT, &ops, 100) != 0)
		return 0;
	ssh_buf_init(&payload, SSH_PACKET_LIMIT, 1);
	while (size > 0) {
		piece = size < 1500 ? size : 1500;
		status = ssh_transport_input(&transport, data, piece);
		data += piece;
		size -= piece;
		if (status != 0 && status != EMSGSIZE)
			break;
		for (;;) {
			ssh_buf_reset(&payload);
			status = ssh_transport_next(&transport, &payload);
			if (status != 0)
				break;
			if (payload.size > 0 && payload.data[0] >= 50)
				(void)ssh_transport_send(&transport, payload.data, payload.size);
		}

		pending = ssh_transport_pending(&transport, &written);
		(void)pending;
		ssh_transport_written(&transport, written);
		(void)ssh_transport_tick(&transport, 100 + SSH_TRANSPORT_REKEY_SECONDS);
		if (transport.state == SSH_TRANSPORT_CLOSED)
			break;
	}

	ssh_buf_free(&payload);
	ssh_transport_free(&transport);
	return 0;
}
