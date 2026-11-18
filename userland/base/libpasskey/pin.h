/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libpasskey's PIN/UV auth protocols 1 and 2 (ws161-p004; CTAP 2.1
 * section 6.5.6 and 6.5.7): the shared secret agreed with the key, and
 * its encrypt, decrypt and authenticate.  Inside the library only.
 */

#ifndef LIBPASSKEY_PIN_H
#define LIBPASSKEY_PIN_H

#include <stddef.h>
#include <stdint.h>

#include "crypto.h"

/* The largest shared secret (protocol 2: the HMAC key, then the AES key), and the platform's COSE key. */
#define PK_PIN_SHARED_MAX	64U
#define PK_PIN_COSE_MAX		96U

/*
 * A secret agreed with a key: the protocol (1 or 2), the secret, and the
 * platform's public key in COSE form (sent with the request that uses it).
 */
struct pk_pin_shared {
	unsigned protocol;
	uint8_t key[PK_PIN_SHARED_MAX];
	size_t key_size;
	uint8_t platform_cose[PK_PIN_COSE_MAX];
	size_t platform_cose_size;
};

int pk_pin_derive(unsigned protocol, const uint8_t *peer_x, const uint8_t *peer_y, struct pk_pin_shared *shared);
int pk_pin_encrypt(const struct pk_pin_shared *shared, const uint8_t *in, size_t size, uint8_t *out, size_t *out_size);
int pk_pin_decrypt(const struct pk_pin_shared *shared, const uint8_t *in, size_t size, uint8_t *out, size_t *out_size);
int pk_pin_authenticate(unsigned protocol, const uint8_t *key, size_t key_size, const uint8_t *message, size_t size,
    uint8_t *out, size_t *out_size);
void pk_pin_wipe(struct pk_pin_shared *shared);

#endif
