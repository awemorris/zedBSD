/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libpasskey's PIN/UV auth protocols (pin.h; ws161-p004).
 *
 *   protocol 1: secret = SHA-256(Z); encrypt = AES-256-CBC with a zero IV;
 *               authenticate = the first 16 bytes of HMAC-SHA-256.
 *   protocol 2: secret = HKDF-SHA-256(32 zero bytes, Z, "CTAP2 HMAC key")
 *               then HKDF-SHA-256(32 zero bytes, Z, "CTAP2 AES key");
 *               encrypt = a random IV, then AES-256-CBC with the AES key;
 *               authenticate = the whole HMAC-SHA-256 with the HMAC key.
 *
 * Z is the x of the ECDH point of the platform's new P-256 key and the
 * key's agreement key.  The platform's key goes with the request in COSE
 * form (kty 2, alg -25, crv 1, x, y).
 */

#include "pin.h"

#include "cbor.h"

#include <errno.h>
#include <string.h>

/* ECDH-ES+HKDF-256, the agreement key's COSE algorithm. */
#define PIN_COSE_ALG_ECDH	(-25)

/* The bytes of protocol 1's MAC, and of a block's IV. */
#define PIN_MAC1_SIZE		16U

static int pin_cose(struct pk_pin_shared *shared, const uint8_t *x, const uint8_t *y);

/*
 * Agrees a secret with a key's agreement key (peer_x, peer_y) for a
 * protocol: a new platform key, the ECDH, and the protocol's derivation.
 * Returns 0, EINVAL for a protocol other than 1 and 2 or a point not on
 * the curve, or EIO.
 */
int
pk_pin_derive(
	unsigned protocol,
	const uint8_t *peer_x,
	const uint8_t *peer_y,
	struct pk_pin_shared *shared)
{
	static const uint8_t zero_salt[PK_SHA256_SIZE] = { 0 };
	struct pk_crypto_part part;
	uint8_t own_x[PK_P256_SIZE];
	uint8_t own_y[PK_P256_SIZE];
	uint8_t z[PK_P256_SIZE];
	int error;

	/* Protocol 1 or 2. */
	memset(shared, 0, sizeof(*shared));
	if (protocol != 1U && protocol != 2U)
		return EINVAL;
	shared->protocol = protocol;

	/* The ECDH with a new key of the platform's. */
	error = pk_crypto_p256_ecdh(peer_x, peer_y, own_x, own_y, z);
	if (error != 0)
		return error;

	/* Protocol 1: SHA-256 of Z. */
	if (protocol == 1U) {
		part.data = z;
		part.size = sizeof(z);
		error = pk_crypto_sha256(&part, 1U, shared->key);
		shared->key_size = PK_SHA256_SIZE;
	} else {
		/* Protocol 2: the HMAC key, then the AES key. */
		error = pk_crypto_hkdf_sha256(zero_salt, sizeof(zero_salt), z, sizeof(z), "CTAP2 HMAC key", shared->key,
		    PK_SHA256_SIZE);
		if (error == 0) {
			error = pk_crypto_hkdf_sha256(zero_salt, sizeof(zero_salt), z, sizeof(z), "CTAP2 AES key",
			    shared->key + PK_SHA256_SIZE, PK_SHA256_SIZE);
		}
		shared->key_size = 2U * PK_SHA256_SIZE;
	}
	pk_crypto_wipe(z, sizeof(z));
	if (error != 0) {
		pk_pin_wipe(shared);
		return error;
	}

	/* The platform's key, for the request. */
	error = pin_cose(shared, own_x, own_y);
	if (error != 0) {
		pk_pin_wipe(shared);
		return error;
	}

	/* Succeeded: the secret is agreed. */
	return 0;
}

/*
 * Encrypts whole blocks under the secret: protocol 1 with a zero IV,
 * protocol 2 with a random IV put before the blocks.  Returns 0, EINVAL
 * for a size that is not whole blocks, or EIO.
 */
int
pk_pin_encrypt(
	const struct pk_pin_shared *shared,
	const uint8_t *in,
	size_t size,
	uint8_t *out,
	size_t *out_size)
{
	static const uint8_t zero_iv[PK_AES_BLOCK_SIZE] = { 0 };
	int error;

	/* Protocol 1: the secret is the key, the IV is zero. */
	if (shared->protocol == 1U) {
		error = pk_crypto_aes256_cbc(1, shared->key, zero_iv, in, size, out);
		if (error != 0)
			return error;
		*out_size = size;
		return 0;
	}

	/* Protocol 2: a random IV first, then the blocks under the AES key. */
	error = pk_crypto_random(out, PK_AES_BLOCK_SIZE);
	if (error != 0)
		return error;
	error = pk_crypto_aes256_cbc(1, shared->key + PK_SHA256_SIZE, out, in, size, out + PK_AES_BLOCK_SIZE);
	if (error != 0)
		return error;

	/* Succeeded: the IV and the blocks. */
	*out_size = PK_AES_BLOCK_SIZE + size;
	return 0;
}

/* Decrypts what pk_pin_encrypt() made (the key's answers).  Returns 0, EINVAL, or EIO. */
int
pk_pin_decrypt(
	const struct pk_pin_shared *shared,
	const uint8_t *in,
	size_t size,
	uint8_t *out,
	size_t *out_size)
{
	static const uint8_t zero_iv[PK_AES_BLOCK_SIZE] = { 0 };
	int error;

	/* Protocol 1: the secret is the key, the IV is zero. */
	if (shared->protocol == 1U) {
		error = pk_crypto_aes256_cbc(0, shared->key, zero_iv, in, size, out);
		if (error != 0)
			return error;
		*out_size = size;
		return 0;
	}

	/* Protocol 2: the IV, then the blocks. */
	if (size < PK_AES_BLOCK_SIZE)
		return EINVAL;
	error = pk_crypto_aes256_cbc(0, shared->key + PK_SHA256_SIZE, in, in + PK_AES_BLOCK_SIZE,
	    size - PK_AES_BLOCK_SIZE, out);
	if (error != 0)
		return error;

	/* Succeeded: the plain blocks. */
	*out_size = size - PK_AES_BLOCK_SIZE;
	return 0;
}

/*
 * Authenticates a message with a key (the secret's HMAC key, or a PIN
 * token): protocol 1 the first 16 bytes of HMAC-SHA-256, protocol 2 all of
 * it.  Returns 0 or EIO.
 */
int
pk_pin_authenticate(
	unsigned protocol,
	const uint8_t *key,
	size_t key_size,
	const uint8_t *message,
	size_t size,
	uint8_t *out,
	size_t *out_size)
{
	uint8_t mac[PK_SHA256_SIZE];
	int error;

	/* The HMAC (protocol 2 keys it with the secret's first half, the HMAC key, which the caller gives). */
	error = pk_crypto_hmac_sha256(key, key_size, message, size, mac);
	if (error != 0)
		return error;

	/* Protocol 1 keeps the first 16 bytes. */
	*out_size = PK_SHA256_SIZE;
	if (protocol == 1U)
		*out_size = PIN_MAC1_SIZE;
	memcpy(out, mac, *out_size);
	pk_crypto_wipe(mac, sizeof(mac));

	/* Succeeded: the MAC. */
	return 0;
}

/* Wipes an agreed secret. */
void
pk_pin_wipe(
	struct pk_pin_shared *shared)
{
	/* Every byte. */
	pk_crypto_wipe(shared, sizeof(*shared));
}

/* Writes the platform's key in COSE form: kty 2, alg -25, crv 1, x, y. */
static int
pin_cose(
	struct pk_pin_shared *shared,
	const uint8_t *x,
	const uint8_t *y)
{
	struct pk_cbor_writer writer;

	/* The map, its labels in canonical order. */
	pk_cbor_writer_init(&writer, shared->platform_cose, sizeof(shared->platform_cose));
	pk_cbor_put_map(&writer, 5U);
	pk_cbor_put_integer(&writer, 1);
	pk_cbor_put_integer(&writer, 2);
	pk_cbor_put_integer(&writer, 3);
	pk_cbor_put_integer(&writer, PIN_COSE_ALG_ECDH);
	pk_cbor_put_integer(&writer, -1);
	pk_cbor_put_integer(&writer, 1);
	pk_cbor_put_integer(&writer, -2);
	pk_cbor_put_bytes(&writer, x, PK_P256_SIZE);
	pk_cbor_put_integer(&writer, -3);
	pk_cbor_put_bytes(&writer, y, PK_P256_SIZE);
	if (writer.error != 0)
		return EIO;

	/* Succeeded: the key's COSE form. */
	shared->platform_cose_size = writer.length;
	return 0;
}
