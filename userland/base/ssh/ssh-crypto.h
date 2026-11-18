/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The cryptographic operations ssh, sshd and ssh-keygen need, in the shape
 * the protocol uses them.  This is the only part that calls the crypto
 * library: ssh-crypto.c includes <compat/crypto/evp.h>,
 * <compat/crypto/rand.h> and <compat/crypto/crypto.h> (the system's
 * libcrypto-compat, whose functions have the names, the arguments and the
 * return conventions of OpenSSL 3.5's libcrypto) and nothing else here
 * does.  No primitive is implemented here: each function is a call, or a
 * short sequence of calls, of that library.
 *
 * The library functions each operation uses are named in its comment, so
 * that the set this file needs is known: RAND_bytes, OPENSSL_cleanse,
 * CRYPTO_memcmp; EVP_MD_CTX_new, EVP_MD_CTX_free, EVP_DigestInit_ex,
 * EVP_DigestUpdate, EVP_DigestFinal_ex, EVP_sha256; EVP_PKEY_new_raw_
 * private_key, EVP_PKEY_new_raw_public_key, EVP_PKEY_new_raw_public_key_ex,
 * EVP_PKEY_get_raw_public_key, EVP_PKEY_free, EVP_DigestSignInit,
 * EVP_DigestSign, EVP_DigestVerifyInit, EVP_DigestVerify;
 * EVP_PKEY_CTX_new, EVP_PKEY_CTX_new_from_name, EVP_PKEY_CTX_new_from_pkey,
 * EVP_PKEY_CTX_free, EVP_PKEY_keygen_init, EVP_PKEY_keygen,
 * EVP_PKEY_derive_init, EVP_PKEY_derive_set_peer, EVP_PKEY_derive,
 * EVP_PKEY_encapsulate_init, EVP_PKEY_encapsulate,
 * EVP_PKEY_decapsulate_init, EVP_PKEY_decapsulate; EVP_CIPHER_CTX_new,
 * EVP_CIPHER_CTX_free, EVP_CipherInit_ex, EVP_CipherUpdate, EVP_chacha20;
 * EVP_MAC_fetch, EVP_MAC_free, EVP_MAC_CTX_new, EVP_MAC_CTX_free,
 * EVP_MAC_init, EVP_MAC_update, EVP_MAC_final.
 *
 * A library call that fails (a return of 0 or less where OpenSSL's
 * convention is 1 for success, or NULL) is EIO unless the comment of the
 * function says otherwise.  Every function wipes the secrets it put in
 * local memory and frees every library object it made, on every path.
 */

#ifndef SSH_SSH_CRYPTO_H
#define SSH_SSH_CRYPTO_H

#include "ssh-common.h"

/* The SHA-256 digest size in bytes (FIPS 180-4). */
#define SSH_SHA256_SIZE		32U

/* The Ed25519 sizes in bytes (RFC 8032 5.1.5, 5.1.6). */
#define SSH_ED25519_SEED	32U
#define SSH_ED25519_PUBLIC	32U
#define SSH_ED25519_SIGNATURE	64U

/* The X25519 size of a private key, a public key and a shared secret (RFC 7748 5). */
#define SSH_X25519_SIZE		32U

/* The ML-KEM-768 sizes in bytes (FIPS 203 Table 3: ek, c, K). */
#define SSH_MLKEM768_PUBLIC	1184U
#define SSH_MLKEM768_CIPHERTEXT	1088U
#define SSH_MLKEM768_SECRET	32U

/*
 * The sizes of the hybrid key exchange's messages (draft-ietf-sshm-mlkem-
 * hybrid-kex-10 2.1 and 2.3.3): C_INIT = C_PK2 || C_PK1 (the ML-KEM-768
 * encapsulation key, then the client's X25519 public key); S_REPLY =
 * S_CT2 || S_PK1 (the ML-KEM-768 ciphertext, then the server's X25519
 * public key).
 */
#define SSH_HYBRID_INIT_SIZE	(SSH_MLKEM768_PUBLIC + SSH_X25519_SIZE)
#define SSH_HYBRID_REPLY_SIZE	(SSH_MLKEM768_CIPHERTEXT + SSH_X25519_SIZE)

/* The ChaCha20 key, the ChaCha20 nonce of the 64-bit-counter form, and the Poly1305 key and tag, in bytes. */
#define SSH_CHACHA20_KEY	32U
#define SSH_CHACHA20_NONCE	8U
#define SSH_POLY1305_KEY	32U
#define SSH_POLY1305_TAG	16U

/*
 * A SHA-256 computation in progress (an exchange hash built from several
 * pieces).
 *
 * context is the library's EVP_MD_CTX, made by ssh_sha256_init and freed
 * by ssh_sha256_final or ssh_sha256_free.  A structure of all zeros
 * (context NULL) holds nothing.
 */
struct ssh_sha256 {
	void *context;
};

/*
 * The client's half of a hybrid key exchange, kept between sending
 * SSH_MSG_KEX_HYBRID_INIT and receiving the reply.
 *
 * mlkem is the library's EVP_PKEY of the ML-KEM-768 key pair (its
 * decapsulation key is secret); x25519_private is the client's X25519
 * private key (secret).  Made by ssh_hybrid_client_start, wiped and freed
 * by ssh_hybrid_client_finish or ssh_hybrid_client_clear.  All zeros holds
 * nothing.
 */
struct ssh_hybrid_client {
	void *mlkem;
	uint8_t x25519_private[SSH_X25519_SIZE];
};

/* Overwrites size bytes at memory with zeros in a way the compiler keeps (OPENSSL_cleanse); memory may be NULL only when size is 0. */
void ssh_wipe(void *memory, size_t size);

/*
 * Compares size bytes in constant time (CRYPTO_memcmp).
 *
 * Returns 1 when equal and 0 otherwise; the time depends only on size.
 */
int ssh_ct_equal(const void *left, const void *right, size_t size);

/*
 * Fills size bytes at out with random bytes (RAND_bytes, in pieces of at
 * most INT_MAX bytes).
 *
 * Returns 0, or EIO when the library failed (out is then wiped).  There is
 * no weaker fallback.
 */
int ssh_random(void *out, size_t size);

/* Starts a SHA-256 computation (EVP_MD_CTX_new, EVP_DigestInit_ex with EVP_sha256).  Returns 0, ENOMEM or EIO. */
int ssh_sha256_init(struct ssh_sha256 *hash);

/* Absorbs size bytes (EVP_DigestUpdate); data may be NULL only when size is 0.  Returns 0 or EIO. */
int ssh_sha256_update(struct ssh_sha256 *hash, const void *data, size_t size);

/*
 * Writes the 32-byte digest to out (EVP_DigestFinal_ex) and frees the
 * computation, also on a failure.  Returns 0 or EIO (out wiped).
 */
int ssh_sha256_final(struct ssh_sha256 *hash, uint8_t *out);

/* Frees an abandoned computation; a zero structure is ignored. */
void ssh_sha256_free(struct ssh_sha256 *hash);

/* Hashes size bytes at data into out (32 bytes) in one call.  Returns 0, ENOMEM or EIO. */
int ssh_sha256(const void *data, size_t size, uint8_t *out);

/*
 * Computes the Ed25519 public key (32 bytes) of a 32-byte seed (RFC 8032
 * 5.1.5): EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519) and
 * EVP_PKEY_get_raw_public_key.  Returns 0 or EIO.
 */
int ssh_ed25519_public(const uint8_t *seed, uint8_t *public_key);

/*
 * Signs size bytes at message with the seed (RFC 8032 5.1.6, pure
 * Ed25519), writing 64 bytes to signature: EVP_PKEY_new_raw_private_key,
 * EVP_DigestSignInit with a NULL digest, EVP_DigestSign.
 *
 * message may be NULL only when size is 0.  Returns 0 or EIO (signature
 * wiped).
 */
int ssh_ed25519_sign(const uint8_t *seed, const uint8_t *message, size_t size, uint8_t *signature);

/*
 * Verifies a 64-byte signature of size bytes at message under a 32-byte
 * public key (RFC 8032 5.1.7): EVP_PKEY_new_raw_public_key,
 * EVP_DigestVerifyInit, EVP_DigestVerify.
 *
 * Returns 0 when it verifies; EBADMSG when the library answers 0 (a bad
 * signature, a public key or an R that does not decode); EINVAL when the
 * public key is refused at import; EIO for another failure.
 */
int ssh_ed25519_verify(const uint8_t *public_key, const uint8_t *message, size_t size, const uint8_t *signature);

/*
 * Starts the client's half of mlkem768x25519-sha256 (draft-ietf-sshm-
 * mlkem-hybrid-kex-10 2.1, 2.3.3) and writes C_INIT
 * (SSH_HYBRID_INIT_SIZE bytes) to c_init.
 *
 * Makes an ML-KEM-768 key pair (EVP_PKEY_CTX_new_from_name "ML-KEM-768",
 * EVP_PKEY_keygen_init, EVP_PKEY_keygen) and writes its encapsulation key
 * (EVP_PKEY_get_raw_public_key, 1184 bytes) as C_PK2; makes an X25519 key
 * pair from 32 random bytes (ssh_random, EVP_PKEY_new_raw_private_key
 * EVP_PKEY_X25519, EVP_PKEY_get_raw_public_key) and writes its public key
 * as C_PK1.  A new pair of each for every key exchange (section 6).
 *
 * client must hold nothing.  Returns 0, ENOMEM or EIO (client then holds
 * nothing and c_init is wiped).
 */
int ssh_hybrid_client_start(struct ssh_hybrid_client *client, uint8_t *c_init);

/*
 * Does the server's half of mlkem768x25519-sha256: checks C_INIT, makes
 * S_REPLY and the shared secret K.
 *
 * c_init_size must be SSH_HYBRID_INIT_SIZE exactly (section 2.1's length
 * check; EINVAL otherwise).  The encapsulation key C_PK2 is imported with
 * EVP_PKEY_new_raw_public_key_ex(NULL, "ML-KEM-768", NULL, ...), which
 * performs the checks of FIPS 203 7.2 (a refusal is EINVAL), and
 * encapsulated to (EVP_PKEY_CTX_new_from_pkey, EVP_PKEY_encapsulate_init
 * with NULL parameters, EVP_PKEY_encapsulate) giving S_CT2 and K_PQ.  An
 * X25519 pair is made as in ssh_hybrid_client_start, giving S_PK1, and
 * K_CL = X25519(server private, C_PK1) (EVP_PKEY_new_raw_public_key of
 * C_PK1, EVP_PKEY_derive_init, EVP_PKEY_derive_set_peer,
 * EVP_PKEY_derive).  The peer's point is refused (EINVAL) when the library
 * refuses it at import or at the derivation (OpenSSL 3.5 fails the
 * derivation of an all-zero result), and also when the derivation
 * succeeds with an all-zero K_CL, which this function checks itself with
 * ssh_ct_equal against 32 zero bytes (RFC 8731 3, RFC 7748 6.1), so the
 * check does not depend on the library.  K_CL is the
 * 32 bytes of the X25519 output as they are (section 2.4: the fixed-size
 * big-endian re-encoding of RFC 8731's integer is those bytes).  Then
 * k = SHA-256(K_PQ || K_CL) (section 2.4).
 *
 * s_reply receives SSH_HYBRID_REPLY_SIZE bytes (S_CT2 || S_PK1) and k 32
 * bytes.  Returns 0, EINVAL (the peer's values are refused: the caller
 * disconnects with SSH_DISCONNECT_KEY_EXCHANGE_FAILED), ENOMEM or EIO.  On
 * an error s_reply and k are wiped.
 */
int ssh_hybrid_server(const uint8_t *c_init, size_t c_init_size, uint8_t *s_reply, uint8_t *k);

/*
 * Finishes the client's half: checks S_REPLY and computes K.
 *
 * s_reply_size must be SSH_HYBRID_REPLY_SIZE exactly (section 2.1;
 * EINVAL otherwise).  K_PQ is decapsulated from S_CT2 with the client's
 * ML-KEM key (EVP_PKEY_CTX_new_from_pkey, EVP_PKEY_decapsulate_init with
 * NULL parameters, EVP_PKEY_decapsulate; ML-KEM's implicit rejection means
 * a forged ciphertext gives a wrong K_PQ, not an error, and the exchange
 * then fails at the signature or the first packet); K_CL = X25519(client
 * private, S_PK1), refused (EINVAL) as in ssh_hybrid_server; k =
 * SHA-256(K_PQ || K_CL).
 *
 * The client's state is wiped and freed whatever the result.  Returns 0,
 * EINVAL, ENOMEM or EIO; on an error k is wiped.
 */
int ssh_hybrid_client_finish(struct ssh_hybrid_client *client, const uint8_t *s_reply, size_t s_reply_size, uint8_t *k);

/* Wipes and frees a client half that will not be finished; a zero structure is ignored. */
void ssh_hybrid_client_clear(struct ssh_hybrid_client *client);

/*
 * XORs size bytes of in with the ChaCha20 keystream of the 64-bit-counter
 * form (draft-ietf-sshm-chacha20-poly1305-04 3: state words 12 and 13 a
 * 64-bit block counter, words 14 and 15 an 8-byte nonce, each word read
 * little-endian) starting at block counter, into out.
 *
 * EVP_chacha20 takes a 16-byte IV whose bytes become state words 12 to 15
 * read little-endian; the IV passed is counter as 8 little-endian bytes,
 * then nonce.  (EVP_CipherInit_ex with the key and that IV, encrypting,
 * then EVP_CipherUpdate.)  counter is 0 or 1 in every use here and size
 * at most SSH_PACKET_LIMIT, so no counter carry question arises.  in and
 * out may be the same buffer.
 *
 * Returns 0 or EIO (out wiped).
 */
int ssh_chacha20_xor(const uint8_t *key, uint32_t counter, const uint8_t *nonce, const uint8_t *in, uint8_t *out, size_t size);

/*
 * Computes the Poly1305 tag (16 bytes) of size bytes at message under a
 * 32-byte one-time key (draft-ietf-sshm-chacha20-poly1305-04 4; the r
 * half first, then the n half, as RFC 8439 2.5): EVP_MAC_fetch(NULL,
 * "POLY1305", NULL), EVP_MAC_CTX_new, EVP_MAC_init with the key,
 * EVP_MAC_update, EVP_MAC_final.
 *
 * message may be NULL only when size is 0.  Returns 0 or EIO (tag wiped).
 */
int ssh_poly1305(const uint8_t *key, const uint8_t *message, size_t size, uint8_t *tag);

#endif
