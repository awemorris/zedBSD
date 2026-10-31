/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libcrypto-compat's EVP interface: the subset of OpenSSL 3.5's that
 * zedBSD's ssh and sshd use.  Names, signatures and return conventions
 * are OpenSSL 3.5's (functions return 1 on success and 0 or a negative
 * value on failure, queuing an error, unless a comment says otherwise);
 * the behaviour described is the subset's, documented here.
 *
 * Algorithms: SHA-256 (EVP_sha256), ChaCha20 (EVP_chacha20), Poly1305
 * (EVP_MAC "POLY1305"), Ed25519 (EVP_PKEY_ED25519) for signatures,
 * X25519 (EVP_PKEY_X25519) for key agreement, and ML-KEM-768 ("ML-KEM-768")
 * for key encapsulation.  Anything else (another digest, cipher, key type
 * or operation) fails.  Every ENGINE, OSSL_LIB_CTX, property query and
 * OSSL_PARAM array argument must be NULL.
 *
 * Objects may be used by one thread at a time each; different objects on
 * different threads need no locking (EVP_PKEY's reference count is
 * atomic).
 */

#ifndef KERN_COMPAT_CRYPTO_EVP_H
#define KERN_COMPAT_CRYPTO_EVP_H

#include <compat/crypto/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The key types and the largest digest.
 * Values referenced: OpenSSL 3.5.8 include/openssl/obj_mac.h (NID_X25519,
 * NID_ED25519) via include/openssl/evp.h (EVP_PKEY_X25519,
 * EVP_PKEY_ED25519, EVP_MAX_MD_SIZE).
 */
#define EVP_PKEY_X25519		1034
#define EVP_PKEY_ED25519	1087
#define EVP_MAX_MD_SIZE		64

/* OpenSSL 3's older name of EVP_MD_get_size. */
#define EVP_MD_size		EVP_MD_get_size

/* Returns the SHA-256 digest algorithm (a constant object). */
const EVP_MD *EVP_sha256(void);

/* Returns a digest's output size in bytes (32 for SHA-256), or -1 for NULL. */
int EVP_MD_get_size(const EVP_MD *md);

/* Makes an empty digest context; NULL without memory. */
EVP_MD_CTX *EVP_MD_CTX_new(void);

/* Wipes and frees a digest context (and drops its key, after a DigestSign/VerifyInit); NULL is ignored. */
void EVP_MD_CTX_free(EVP_MD_CTX *ctx);

/* Starts a digest with type (EVP_sha256 only; impl NULL), discarding what ctx held. */
int EVP_DigestInit_ex(EVP_MD_CTX *ctx, const EVP_MD *type, ENGINE *impl);

/* Absorbs count bytes. */
int EVP_DigestUpdate(EVP_MD_CTX *ctx, const void *data, size_t count);

/* Writes the digest to md, its size to *size when size is not NULL; ctx must be started again for reuse. */
int EVP_DigestFinal_ex(EVP_MD_CTX *ctx, unsigned char *md, unsigned int *size);

/* Makes out a copy of the digest in progress in in (a snapshot); out need not be started. */
int EVP_MD_CTX_copy_ex(EVP_MD_CTX *out, const EVP_MD_CTX *in);

/* Hashes count bytes in one call into md (and its size into *size when not NULL). */
int EVP_Digest(const void *data, size_t count, unsigned char *md, unsigned int *size, const EVP_MD *type, ENGINE *impl);

/*
 * Makes a key from its raw private form: for EVP_PKEY_ED25519 the 32-byte
 * seed of RFC 8032 5.1.5, for EVP_PKEY_X25519 the 32-byte scalar of RFC
 * 7748 (unclamped bytes, as received).  The public key is computed.
 * Returns the key with one reference, or NULL (wrong type or length, no
 * memory; e must be NULL).
 */
EVP_PKEY *EVP_PKEY_new_raw_private_key(int type, ENGINE *e, const unsigned char *key, size_t length);

/* Makes a public key from its raw form (32 bytes for both types), or NULL. */
EVP_PKEY *EVP_PKEY_new_raw_public_key(int type, ENGINE *e, const unsigned char *key, size_t length);

/*
 * Makes a public key of the named type from its raw form: "ML-KEM-768"
 * with the 1184-byte encapsulation key, which must pass the modulus check
 * of FIPS 203 7.2 (else NULL); "ED25519" and "X25519" as
 * EVP_PKEY_new_raw_public_key.  libctx and propq must be NULL.
 */
EVP_PKEY *EVP_PKEY_new_raw_public_key_ex(OSSL_LIB_CTX *libctx, const char *keytype, const char *propq, const unsigned char *key, size_t length);

/*
 * Writes a key's raw public form (32 bytes; 1184 for ML-KEM-768's
 * encapsulation key).  With key NULL, sets *length to the size and
 * returns 1; otherwise *length is the room on entry (too small: 0) and
 * the size on return.
 */
int EVP_PKEY_get_raw_public_key(const EVP_PKEY *pkey, unsigned char *key, size_t *length);

/*
 * Writes a key's raw private form (32 bytes: Ed25519's seed, X25519's
 * scalar), as EVP_PKEY_get_raw_public_key; 0 for a public-only key and
 * for ML-KEM-768 (its private form is not exported).
 */
int EVP_PKEY_get_raw_private_key(const EVP_PKEY *pkey, unsigned char *key, size_t *length);

/* Drops one reference to a key, wiping and freeing it at the last; NULL is ignored. */
void EVP_PKEY_free(EVP_PKEY *pkey);

/*
 * Starts an Ed25519 signature with pkey (which must have its private
 * key): type must be NULL (pure Ed25519 hashes internally), pctx NULL or
 * receives NULL, e NULL.  ctx takes a reference to pkey.
 */
int EVP_DigestSignInit(EVP_MD_CTX *ctx, EVP_PKEY_CTX **pctx, const EVP_MD *type, ENGINE *e, EVP_PKEY *pkey);

/*
 * Signs tbs (one shot, RFC 8032 5.1.6).  With signature NULL sets
 * *length to 64 and returns 1; otherwise *length is the room (at least
 * 64) and becomes 64.
 */
int EVP_DigestSign(EVP_MD_CTX *ctx, unsigned char *signature, size_t *length, const unsigned char *tbs, size_t tbs_length);

/* Starts an Ed25519 verification with pkey (public key enough); arguments as EVP_DigestSignInit. */
int EVP_DigestVerifyInit(EVP_MD_CTX *ctx, EVP_PKEY_CTX **pctx, const EVP_MD *type, ENGINE *e, EVP_PKEY *pkey);

/*
 * Verifies a signature over tbs (one shot, RFC 8032 5.1.7).  Returns 1
 * when it verifies, 0 when it does not (including a signature of a length
 * other than 64), and a negative value for a context not started.
 */
int EVP_DigestVerify(EVP_MD_CTX *ctx, const unsigned char *signature, size_t length, const unsigned char *tbs, size_t tbs_length);

/* Makes an operation context on pkey (taking a reference); e must be NULL.  NULL on failure. */
EVP_PKEY_CTX *EVP_PKEY_CTX_new(EVP_PKEY *pkey, ENGINE *e);

/* Makes a key-generation context for a type named "ML-KEM-768" (libctx and propquery NULL).  NULL otherwise. */
EVP_PKEY_CTX *EVP_PKEY_CTX_new_from_name(OSSL_LIB_CTX *libctx, const char *name, const char *propquery);

/* As EVP_PKEY_CTX_new (libctx and propquery NULL). */
EVP_PKEY_CTX *EVP_PKEY_CTX_new_from_pkey(OSSL_LIB_CTX *libctx, EVP_PKEY *pkey, const char *propquery);

/* Frees a context and drops its keys; NULL is ignored. */
void EVP_PKEY_CTX_free(EVP_PKEY_CTX *ctx);

/* Prepares an X25519 derivation with the context's private key. */
int EVP_PKEY_derive_init(EVP_PKEY_CTX *ctx);

/* Sets the peer's X25519 public key (the context takes a reference). */
int EVP_PKEY_derive_set_peer(EVP_PKEY_CTX *ctx, EVP_PKEY *peer);

/*
 * Derives the X25519 shared secret (RFC 7748 6.1).  With key NULL sets
 * *length to 32 and returns 1.  An all-zero result fails (returns 0,
 * nothing usable written: key is zeroed).
 */
int EVP_PKEY_derive(EVP_PKEY_CTX *ctx, unsigned char *key, size_t *length);

/* Prepares a key generation on a context from EVP_PKEY_CTX_new_from_name. */
int EVP_PKEY_keygen_init(EVP_PKEY_CTX *ctx);

/* Generates an ML-KEM-768 key pair (FIPS 203 Algorithm 19) into a new key in *pkey. */
int EVP_PKEY_keygen(EVP_PKEY_CTX *ctx, EVP_PKEY **pkey);

/* Prepares an encapsulation to the context's ML-KEM-768 public key; params must be NULL. */
int EVP_PKEY_encapsulate_init(EVP_PKEY_CTX *ctx, const OSSL_PARAM params[]);

/*
 * Encapsulates (FIPS 203 Algorithm 20): writes the 1088-byte ciphertext
 * to wrapped and the 32-byte shared key to key.  With both NULL sets the
 * two lengths and returns 1; otherwise each length is the room (too
 * small: 0) and becomes the size.
 */
int EVP_PKEY_encapsulate(EVP_PKEY_CTX *ctx, unsigned char *wrapped, size_t *wrapped_length, unsigned char *key, size_t *key_length);

/* Prepares a decapsulation with the context's ML-KEM-768 private key; params must be NULL. */
int EVP_PKEY_decapsulate_init(EVP_PKEY_CTX *ctx, const OSSL_PARAM params[]);

/*
 * Decapsulates (FIPS 203 Algorithm 21): writes the 32-byte shared key.
 * With key NULL sets *length to 32 and returns 1.  A ciphertext of a
 * length other than 1088 fails; a wrong ciphertext of the right length
 * succeeds with the implicit-rejection key.
 */
int EVP_PKEY_decapsulate(EVP_PKEY_CTX *ctx, unsigned char *key, size_t *length, const unsigned char *wrapped, size_t wrapped_length);

/* Returns the ChaCha20 cipher (a constant object): 32-byte key, 16-byte IV. */
const EVP_CIPHER *EVP_chacha20(void);

/* Makes an empty cipher context; NULL without memory. */
EVP_CIPHER_CTX *EVP_CIPHER_CTX_new(void);

/* Wipes and frees a cipher context; NULL is ignored. */
void EVP_CIPHER_CTX_free(EVP_CIPHER_CTX *ctx);

/*
 * Sets up ChaCha20: cipher EVP_chacha20 (or NULL to keep the one set),
 * key 32 bytes (or NULL to keep it), iv 16 bytes (or NULL to keep it),
 * enc 1, 0 or -1 (keep; ChaCha20 is the same both ways).  The IV is
 * OpenSSL's layout: bytes 0 to 15 are the block function's state words
 * 12 to 15, little-endian; libcrypto-compat counts blocks in words 12 and
 * 13 as one 64-bit counter (OpenSSL carries only within word 12; the two
 * differ only after 2^32 blocks).  Setting an IV restarts the keystream.
 */
int EVP_CipherInit_ex(EVP_CIPHER_CTX *ctx, const EVP_CIPHER *cipher, ENGINE *impl, const unsigned char *key, const unsigned char *iv, int enc);

/*
 * XORs in_length bytes of in with the keystream into out (out may be in),
 * continuing the keystream across calls; *out_length becomes in_length.
 */
int EVP_CipherUpdate(EVP_CIPHER_CTX *ctx, unsigned char *out, int *out_length, const unsigned char *in, int in_length);

/* As EVP_CipherUpdate without the length output; returns 1 or 0 (OpenSSL's convention for stream ciphers). */
int EVP_Cipher(EVP_CIPHER_CTX *ctx, unsigned char *out, const unsigned char *in, unsigned int length);

/* Returns the MAC algorithm named "POLY1305" (libctx and properties NULL), or NULL for another name. */
EVP_MAC *EVP_MAC_fetch(OSSL_LIB_CTX *libctx, const char *algorithm, const char *properties);

/* Releases a MAC algorithm; NULL is ignored. */
void EVP_MAC_free(EVP_MAC *mac);

/* Makes a MAC context for mac; NULL on failure. */
EVP_MAC_CTX *EVP_MAC_CTX_new(EVP_MAC *mac);

/* Wipes and frees a MAC context; NULL is ignored. */
void EVP_MAC_CTX_free(EVP_MAC_CTX *ctx);

/* (Re)starts Poly1305 with a 32-byte one-time key (params NULL); may be called again on the same context. */
int EVP_MAC_init(EVP_MAC_CTX *ctx, const unsigned char *key, size_t length, const OSSL_PARAM params[]);

/* Absorbs length bytes. */
int EVP_MAC_update(EVP_MAC_CTX *ctx, const unsigned char *data, size_t length);

/* Writes the 16-byte tag (out_size at least 16; *out_length 16); the context needs EVP_MAC_init before reuse. */
int EVP_MAC_final(EVP_MAC_CTX *ctx, unsigned char *out, size_t *out_length, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif
