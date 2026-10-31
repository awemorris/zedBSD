/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The cryptographic operations of ssh and sshd, as calls of the system crypto library.
 *
 * Skeleton: every function answers ENOSYS (or does nothing) until it is
 * implemented.
 */

#include "ssh-crypto.h"

/*
 * Overwrites size bytes at memory with zeros in a way the compiler keeps
 * (OPENSSL_cleanse); memory may be NULL only when size is 0.
 */
void
ssh_wipe(
	void *memory,
	size_t size)
{
	/* Marks the arguments used until the function is implemented. */
	(void)memory;
	(void)size;
}

/*
 * Compares size bytes in constant time (CRYPTO_memcmp).
 */
int
ssh_ct_equal(
	const void *left,
	const void *right,
	size_t size)
{
	/* Marks the arguments used until the function is implemented. */
	(void)left;
	(void)right;
	(void)size;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Fills size bytes at out with random bytes (RAND_bytes, in pieces of at
 * most INT_MAX bytes).
 */
int
ssh_random(
	void *out,
	size_t size)
{
	/* Marks the arguments used until the function is implemented. */
	(void)out;
	(void)size;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Starts a SHA-256 computation (EVP_MD_CTX_new, EVP_DigestInit_ex with
 * EVP_sha256).
 */
int
ssh_sha256_init(
	struct ssh_sha256 *hash)
{
	/* Marks the arguments used until the function is implemented. */
	(void)hash;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Absorbs size bytes (EVP_DigestUpdate); data may be NULL only when size
 * is 0.
 */
int
ssh_sha256_update(
	struct ssh_sha256 *hash,
	const void *data,
	size_t size)
{
	/* Marks the arguments used until the function is implemented. */
	(void)hash;
	(void)data;
	(void)size;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Writes the 32-byte digest to out (EVP_DigestFinal_ex) and frees the
 * computation, also on a failure.
 */
int
ssh_sha256_final(
	struct ssh_sha256 *hash,
	uint8_t *out)
{
	/* Marks the arguments used until the function is implemented. */
	(void)hash;
	(void)out;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Frees an abandoned computation; a zero structure is ignored.
 */
void
ssh_sha256_free(
	struct ssh_sha256 *hash)
{
	/* Marks the arguments used until the function is implemented. */
	(void)hash;
}

/*
 * Hashes size bytes at data into out (32 bytes) in one call.
 */
int
ssh_sha256(
	const void *data,
	size_t size,
	uint8_t *out)
{
	/* Marks the arguments used until the function is implemented. */
	(void)data;
	(void)size;
	(void)out;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Computes the Ed25519 public key (32 bytes) of a 32-byte seed (RFC 8032
 * 5.1.5): EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519) and
 * EVP_PKEY_get_raw_public_key.
 */
int
ssh_ed25519_public(
	const uint8_t *seed,
	uint8_t *public_key)
{
	/* Marks the arguments used until the function is implemented. */
	(void)seed;
	(void)public_key;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Signs size bytes at message with the seed (RFC 8032 5.1.6, pure
 * Ed25519), writing 64 bytes to signature: EVP_PKEY_new_raw_private_key,
 * EVP_DigestSignInit with a NULL digest, EVP_DigestSign.
 */
int
ssh_ed25519_sign(
	const uint8_t *seed,
	const uint8_t *message,
	size_t size,
	uint8_t *signature)
{
	/* Marks the arguments used until the function is implemented. */
	(void)seed;
	(void)message;
	(void)size;
	(void)signature;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Verifies a 64-byte signature of size bytes at message under a 32-byte
 * public key (RFC 8032 5.1.7): EVP_PKEY_new_raw_public_key,
 * EVP_DigestVerifyInit, EVP_DigestVerify.
 */
int
ssh_ed25519_verify(
	const uint8_t *public_key,
	const uint8_t *message,
	size_t size,
	const uint8_t *signature)
{
	/* Marks the arguments used until the function is implemented. */
	(void)public_key;
	(void)message;
	(void)size;
	(void)signature;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Starts the client's half of mlkem768x25519-sha256 (draft-ietf-sshm-
 * mlkem-hybrid-kex-10 2.1, 2.3.3) and writes C_INIT
 * (SSH_HYBRID_INIT_SIZE bytes) to c_init.
 */
int
ssh_hybrid_client_start(
	struct ssh_hybrid_client *client,
	uint8_t *c_init)
{
	/* Marks the arguments used until the function is implemented. */
	(void)client;
	(void)c_init;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Does the server's half of mlkem768x25519-sha256: checks C_INIT, makes
 * S_REPLY and the shared secret K.
 */
int
ssh_hybrid_server(
	const uint8_t *c_init,
	size_t c_init_size,
	uint8_t *s_reply,
	uint8_t *k)
{
	/* Marks the arguments used until the function is implemented. */
	(void)c_init;
	(void)c_init_size;
	(void)s_reply;
	(void)k;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Finishes the client's half: checks S_REPLY and computes K.
 */
int
ssh_hybrid_client_finish(
	struct ssh_hybrid_client *client,
	const uint8_t *s_reply,
	size_t s_reply_size,
	uint8_t *k)
{
	/* Marks the arguments used until the function is implemented. */
	(void)client;
	(void)s_reply;
	(void)s_reply_size;
	(void)k;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Wipes and frees a client half that will not be finished; a zero
 * structure is ignored.
 */
void
ssh_hybrid_client_clear(
	struct ssh_hybrid_client *client)
{
	/* Marks the arguments used until the function is implemented. */
	(void)client;
}

/*
 * XORs size bytes of in with the ChaCha20 keystream of the
 * 64-bit-counter form (draft-ietf-sshm-chacha20-poly1305-04 3: state
 * words 12 and 13 a 64-bit block counter, words 14 and 15 an 8-byte
 * nonce, each word read little-endian) starting at block counter, into
 * out.
 */
int
ssh_chacha20_xor(
	const uint8_t *key,
	uint32_t counter,
	const uint8_t *nonce,
	const uint8_t *in,
	uint8_t *out,
	size_t size)
{
	/* Marks the arguments used until the function is implemented. */
	(void)key;
	(void)counter;
	(void)nonce;
	(void)in;
	(void)out;
	(void)size;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Computes the Poly1305 tag (16 bytes) of size bytes at message under a
 * 32-byte one-time key (draft-ietf-sshm-chacha20-poly1305-04 4; the r
 * half first, then the n half, as RFC 8439 2.5): EVP_MAC_fetch(NULL,
 * "POLY1305", NULL), EVP_MAC_CTX_new, EVP_MAC_init with the key,
 * EVP_MAC_update, EVP_MAC_final.
 */
int
ssh_poly1305(
	const uint8_t *key,
	const uint8_t *message,
	size_t size,
	uint8_t *tag)
{
	/* Marks the arguments used until the function is implemented. */
	(void)key;
	(void)message;
	(void)size;
	(void)tag;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}
