/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libpasskey's cryptography on OpenSSL's libcrypto (crypto.h; ws161-p004).
 *
 * This is the Guardrail's exception for /sbin/passkey (ws172): base code
 * calls the package's libcrypto until the release, when an implementation
 * of zedBSD's own replaces this file.  Only OpenSSL 3's EVP interfaces are
 * used.  Every function answers 0, or an errno value: EACCES for a
 * signature that does not verify, EINVAL for an input OpenSSL refuses
 * (a point not on the curve), EIO for anything else.
 */

#include "crypto.h"

#include <errno.h>
#include <string.h>

#include <compat/crypto/core_names.h>
#include <compat/crypto/crypto.h>
#include <compat/crypto/evp.h>
#include <compat/crypto/hmac.h>
#include <compat/crypto/kdf.h>
#include <compat/crypto/params.h>
#include <compat/crypto/rand.h>

/* An uncompressed P-256 point: 0x04, then x and y. */
#define CRYPTO_POINT_SIZE	(1U + 2U * PK_P256_SIZE)

static EVP_PKEY *crypto_p256_key(const uint8_t *x, const uint8_t *y);

/* Hashes the parts of a message, in order, with SHA-256. */
int
pk_crypto_sha256(
	const struct pk_crypto_part *parts,
	size_t count,
	uint8_t *hash)
{
	EVP_MD_CTX *context;
	unsigned int length;
	size_t index;
	int good;

	/* The digest's context. */
	context = EVP_MD_CTX_new();
	if (context == NULL)
		return EIO;

	/* The parts, then the hash. */
	good = EVP_DigestInit_ex(context, EVP_sha256(), NULL);
	for (index = 0U; good == 1 && index < count; index++)
		good = EVP_DigestUpdate(context, parts[index].data, parts[index].size);
	length = 0U;
	if (good == 1)
		good = EVP_DigestFinal_ex(context, hash, &length);
	EVP_MD_CTX_free(context);

	/* Reports a digest that failed. */
	if (good != 1 || length != PK_SHA256_SIZE)
		return EIO;

	/* Succeeded: the hash. */
	return 0;
}

/* Computes HMAC-SHA-256 of data under a key. */
int
pk_crypto_hmac_sha256(
	const uint8_t *key,
	size_t key_size,
	const uint8_t *data,
	size_t size,
	uint8_t *mac)
{
	unsigned int length;
	unsigned char *made;

	/* The one-shot HMAC. */
	length = 0U;
	made = HMAC(EVP_sha256(), key, (int)key_size, data, size, mac, &length);
	if (made == NULL || length != PK_SHA256_SIZE)
		return EIO;

	/* Succeeded: the MAC. */
	return 0;
}

/* Fills bytes from the system's random source (through OpenSSL's generator). */
int
pk_crypto_random(
	uint8_t *bytes,
	size_t size)
{
	int good;

	/* The bytes. */
	good = RAND_bytes(bytes, (int)size);
	if (good != 1)
		return EIO;

	/* Succeeded: random bytes. */
	return 0;
}

/* Tells whether (x, y) is a point of P-256 (1), or not (0). */
int
pk_crypto_p256_valid(
	const uint8_t *x,
	const uint8_t *y)
{
	EVP_PKEY *key;

	/* The key, made only from a point on the curve. */
	key = crypto_p256_key(x, y);
	if (key == NULL)
		return 0;
	EVP_PKEY_free(key);

	/* On the curve. */
	return 1;
}

/*
 * Verifies an ECDSA signature (DER, as CTAP2 gives it) of a SHA-256 hash
 * under a P-256 public key (x, y).  Returns 0, EACCES when it does not
 * verify, EINVAL for a key not on the curve, or EIO.
 */
int
pk_crypto_p256_verify(
	const uint8_t *x,
	const uint8_t *y,
	const uint8_t *hash,
	const uint8_t *signature,
	size_t signature_size)
{
	EVP_PKEY_CTX *context;
	EVP_PKEY *key;
	int good;

	/* The public key. */
	key = crypto_p256_key(x, y);
	if (key == NULL)
		return EINVAL;

	/* The verification's context. */
	context = EVP_PKEY_CTX_new(key, NULL);
	if (context == NULL) {
		EVP_PKEY_free(key);
		return EIO;
	}

	/* The signature over the hash (1 good, 0 bad, below 0 an error). */
	good = EVP_PKEY_verify_init(context);
	if (good == 1)
		good = EVP_PKEY_CTX_set_signature_md(context, EVP_sha256());
	if (good == 1)
		good = EVP_PKEY_verify(context, signature, signature_size, hash, PK_SHA256_SIZE);
	EVP_PKEY_CTX_free(context);
	EVP_PKEY_free(key);

	/* A signature that does not verify, or a malformed one. */
	if (good != 1)
		return EACCES;

	/* Succeeded: the signature is good. */
	return 0;
}

/*
 * Agrees a secret with a peer's P-256 key (x, y): makes a new key of our
 * own, gives its public point (own_x, own_y) and the shared secret (the x
 * of the shared point, 32 bytes).  Returns 0, EINVAL for a peer point not
 * on the curve, or EIO.
 */
int
pk_crypto_p256_ecdh(
	const uint8_t *peer_x,
	const uint8_t *peer_y,
	uint8_t *own_x,
	uint8_t *own_y,
	uint8_t *shared)
{
	EVP_PKEY_CTX *context;
	EVP_PKEY *peer;
	EVP_PKEY *own;
	uint8_t point[CRYPTO_POINT_SIZE];
	size_t length;
	int good;

	/* The peer's key, checked to be on the curve. */
	peer = crypto_p256_key(peer_x, peer_y);
	if (peer == NULL)
		return EINVAL;

	/* A new key of our own. */
	own = EVP_PKEY_Q_keygen(NULL, NULL, "EC", "P-256");
	if (own == NULL) {
		EVP_PKEY_free(peer);
		return EIO;
	}

	/* Our public point, uncompressed. */
	length = 0U;
	good = EVP_PKEY_get_octet_string_param(own, OSSL_PKEY_PARAM_PUB_KEY, point, sizeof(point), &length);
	if (good != 1 || length != CRYPTO_POINT_SIZE || point[0] != 0x04U) {
		EVP_PKEY_free(own);
		EVP_PKEY_free(peer);
		return EIO;
	}

	/* Its coordinates, for the peer. */
	memcpy(own_x, point + 1U, PK_P256_SIZE);
	memcpy(own_y, point + 1U + PK_P256_SIZE, PK_P256_SIZE);

	/* The shared secret. */
	context = EVP_PKEY_CTX_new(own, NULL);
	good = 0;
	if (context != NULL)
		good = EVP_PKEY_derive_init(context);
	if (good == 1)
		good = EVP_PKEY_derive_set_peer(context, peer);
	length = PK_P256_SIZE;
	if (good == 1)
		good = EVP_PKEY_derive(context, shared, &length);
	EVP_PKEY_CTX_free(context);
	EVP_PKEY_free(own);
	EVP_PKEY_free(peer);

	/* Reports an agreement that failed. */
	if (good != 1 || length != PK_P256_SIZE)
		return EIO;

	/* Succeeded: the shared secret. */
	return 0;
}

/*
 * Encrypts or decrypts whole blocks with AES-256-CBC and no padding (the
 * PIN/UV protocols' cipher).  Returns 0, EINVAL for a size that is not
 * whole blocks, or EIO.
 */
int
pk_crypto_aes256_cbc(
	int encrypt,
	const uint8_t *key,
	const uint8_t *iv,
	const uint8_t *in,
	size_t size,
	uint8_t *out)
{
	EVP_CIPHER_CTX *context;
	int direction;
	int written;
	int last;
	int good;

	/* Whole blocks only. */
	if (size % PK_AES_BLOCK_SIZE != 0U)
		return EINVAL;

	/* The cipher's context. */
	context = EVP_CIPHER_CTX_new();
	if (context == NULL)
		return EIO;

	/* The blocks, without padding. */
	written = 0;
	last = 0;
	direction = 0;
	if (encrypt)
		direction = 1;
	good = EVP_CipherInit_ex(context, EVP_aes_256_cbc(), NULL, key, iv, direction);
	if (good == 1)
		good = EVP_CIPHER_CTX_set_padding(context, 0);
	if (good == 1)
		good = EVP_CipherUpdate(context, out, &written, in, (int)size);
	if (good == 1)
		good = EVP_CipherFinal_ex(context, out + written, &last);
	EVP_CIPHER_CTX_free(context);

	/* Reports a cipher that failed. */
	if (good != 1 || (size_t)(written + last) != size)
		return EIO;

	/* Succeeded: the blocks. */
	return 0;
}

/* Derives out_size bytes with HKDF-SHA-256 (RFC 5869) from a secret, a salt and an info text. */
int
pk_crypto_hkdf_sha256(
	const uint8_t *salt,
	size_t salt_size,
	const uint8_t *secret,
	size_t secret_size,
	const char *info,
	uint8_t *out,
	size_t out_size)
{
	OSSL_PARAM parameters[5];
	EVP_KDF_CTX *context;
	EVP_KDF *kdf;
	int good;

	/* HKDF. */
	kdf = EVP_KDF_fetch(NULL, "HKDF", NULL);
	if (kdf == NULL)
		return EIO;
	context = EVP_KDF_CTX_new(kdf);
	EVP_KDF_free(kdf);
	if (context == NULL)
		return EIO;

	/* SHA-256, the secret, the salt and the info. */
	parameters[0] = OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST, (char *)"SHA256", 0U);
	parameters[1] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY, (void *)secret, secret_size);
	parameters[2] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, (void *)salt, salt_size);
	parameters[3] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO, (void *)info, strlen(info));
	parameters[4] = OSSL_PARAM_construct_end();
	good = EVP_KDF_derive(context, out, out_size, parameters);
	EVP_KDF_CTX_free(context);

	/* Reports a derivation that failed. */
	if (good != 1)
		return EIO;

	/* Succeeded: the derived bytes. */
	return 0;
}

/* Wipes a secret in a way the compiler does not leave out. */
void
pk_crypto_wipe(
	void *memory,
	size_t size)
{
	/* OpenSSL's own wipe. */
	OPENSSL_cleanse(memory, size);
}

/* Compares two byte strings in a time that does not depend on where they differ; 1 when equal. */
int
pk_crypto_equal(
	const uint8_t *left,
	const uint8_t *right,
	size_t size)
{
	int compared;

	/* The constant-time comparison. */
	compared = CRYPTO_memcmp(left, right, size);
	if (compared != 0)
		return 0;

	/* Equal. */
	return 1;
}

/* Makes a P-256 public key from its coordinates; NULL for a point not on the curve. */
static EVP_PKEY *
crypto_p256_key(
	const uint8_t *x,
	const uint8_t *y)
{
	OSSL_PARAM parameters[3];
	EVP_PKEY_CTX *context;
	EVP_PKEY_CTX *check;
	EVP_PKEY *key;
	uint8_t point[CRYPTO_POINT_SIZE];
	int good;

	/* The uncompressed point. */
	point[0] = 0x04U;
	memcpy(point + 1U, x, PK_P256_SIZE);
	memcpy(point + 1U + PK_P256_SIZE, y, PK_P256_SIZE);

	/* The key from the curve's name and the point. */
	parameters[0] = OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, (char *)"prime256v1", 0U);
	parameters[1] = OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_PUB_KEY, point, sizeof(point));
	parameters[2] = OSSL_PARAM_construct_end();
	context = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
	if (context == NULL)
		return NULL;
	key = NULL;
	good = EVP_PKEY_fromdata_init(context);
	if (good == 1)
		good = EVP_PKEY_fromdata(context, &key, EVP_PKEY_PUBLIC_KEY, parameters);
	EVP_PKEY_CTX_free(context);
	if (good != 1 || key == NULL)
		return NULL;

	/* The point is on the curve. */
	check = EVP_PKEY_CTX_new(key, NULL);
	good = 0;
	if (check != NULL)
		good = EVP_PKEY_public_check(check);
	EVP_PKEY_CTX_free(check);
	if (good != 1) {
		EVP_PKEY_free(key);
		return NULL;
	}

	/* Succeeded: the key. */
	return key;
}
