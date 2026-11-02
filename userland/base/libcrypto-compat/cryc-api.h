/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The structures behind libcrypto-compat's public object types (the
 * opaque X509, X509_STORE, EVP_PKEY, ... of <compat/crypto/types.h>), and
 * the private functions libssl-compat uses on them.  Shared by the
 * api-*.c files of libcrypto-compat and by libssl-compat's API layer; not
 * installed.
 *
 * Reference counts are changed only with cryc_os_atomic_add.  An object
 * is freed by the call that drops its count to 0.
 */

#ifndef LIBCRYPTO_COMPAT_CRYC_API_H
#define LIBCRYPTO_COMPAT_CRYC_API_H

#include <compat/crypto/core_names.h>
#include <compat/crypto/crypto.h>
#include <compat/crypto/err.h>
#include <compat/crypto/evp.h>
#include <compat/crypto/hmac.h>
#include <compat/crypto/kdf.h>
#include <compat/crypto/params.h>
#include <compat/crypto/rand.h>
#include <compat/crypto/x509.h>

#include "cryc-aes.h"
#include "cryc-chain.h"
#include "cryc-chacha.h"
#include "cryc-ecc.h"
#include "cryc-ed25519.h"
#include "cryc-err.h"
#include "cryc-hash.h"
#include "cryc-mlkem.h"
#include "cryc-x25519.h"

/*
 * A certificate: an owned copy of its DER and its parsed form (views into
 * der).  Made by d2i_X509 and by libssl-compat for the server's chain.
 */
struct x509_st {
	volatile long references;
	uint8_t *der;
	size_t der_size;
	struct cryc_x509 cert;
};

/* A trust store: the internal store (with its own lock). */
struct x509_store_st {
	volatile long references;
	struct cryc_store store;
};

/*
 * Verification parameters: the internal ones (host, address, time).
 * Owned by an SSL (SSL_get0_param gives a pointer into it); not
 * reference counted.
 */
struct X509_VERIFY_PARAM_st {
	struct cryc_verify_params params;
};

/*
 * What a verify callback is given: the verdict as an X509_V_ code and the
 * depth it concerns.  Valid only during the callback.  (No accessor is
 * public in this subset; a callback that ignores its argument works.)
 */
struct x509_store_ctx_st {
	long error;
	int depth;
};

/* The digest algorithm: only SHA-256. */
struct evp_md_st {
	enum cryc_hash_id hash;
	int size;
};

/*
 * A digest context, or a signing or verification context.  mode is
 * CRYC_MD_DIGEST (hash in use), CRYC_MD_SIGN or CRYC_MD_VERIFY (key holds a
 * reference), or CRYC_MD_NONE.
 */
struct evp_md_ctx_st {
	int mode;
	const EVP_MD *md;
	struct cryc_hash hash;
	EVP_PKEY *key;
};

#define CRYC_MD_NONE	0
#define CRYC_MD_DIGEST	1
#define CRYC_MD_SIGN	2
#define CRYC_MD_VERIFY	3

/* A cipher algorithm: ChaCha20 or AES-256-CBC (told apart by the constant object's address). */
struct evp_cipher_st {
	int key_size;
	int iv_size;
};

/*
 * A cipher context.  cipher names the algorithm and key_set says whether
 * a key was given.
 *
 * ChaCha20: key, the 64-bit block counter and the 8-byte nonce from the
 * IV, and the unused rest of the current keystream block
 * (keystream[64 - available .. 63]).
 *
 * AES-256-CBC: aes is the expanded key, chain the IV and then the last
 * ciphertext block, partial[0 .. partial_size - 1] the bytes of an
 * incomplete block kept for the next call, encrypt the direction (1
 * encrypts, 0 decrypts), padding 1 while padding is on.
 *
 * Secret, all of it but the sizes and the flags.
 */
struct evp_cipher_ctx_st {
	const EVP_CIPHER *cipher;
	uint8_t key[CRYC_CHACHA20_KEY];
	int key_set;
	uint64_t counter;
	uint8_t nonce[8];
	uint8_t keystream[CRYC_CHACHA20_BLOCK];
	size_t available;
	struct cryc_aes aes;
	uint8_t chain[CRYC_AES_BLOCK];
	uint8_t partial[CRYC_AES_BLOCK];
	size_t partial_size;
	int encrypt;
	int padding;
};

/* The MAC algorithm: only Poly1305. */
struct evp_mac_st {
	int tag_size;
};

/* A Poly1305 context; started is 1 between EVP_MAC_init and EVP_MAC_final.  Secret. */
struct evp_mac_ctx_st {
	struct cryc_poly1305 poly;
	int started;
};

/* The key types of struct evp_pkey_st. */
#define CRYC_PKEY_ED25519	1
#define CRYC_PKEY_X25519	2
#define CRYC_PKEY_MLKEM768	3
#define CRYC_PKEY_EC_P256	4

/* The size of a P-256 key's public form, the uncompressed point (SEC 1 2.3.3), and of its private scalar. */
#define CRYC_PKEY_P256_POINT	65U
#define CRYC_PKEY_P256_SCALAR	32U

/*
 * A key.  public_key holds the raw public form (32 bytes, or 1184 for
 * ML-KEM-768, or a P-256 key's uncompressed point of 65 bytes, already
 * checked to be on the curve); private_key, when has_private, is a
 * malloc'd raw private form (32 bytes: Ed25519's seed, X25519's scalar or
 * P-256's big-endian scalar d; 2400 bytes: ML-KEM-768's dk), wiped before
 * it is freed.
 */
struct evp_pkey_st {
	volatile long references;
	int type;
	uint8_t public_key[CRYC_MLKEM768_EK];
	size_t public_size;
	int has_private;
	uint8_t *private_key;
	size_t private_size;
};

/* The operations of struct evp_pkey_ctx_st. */
#define CRYC_OP_NONE		0
#define CRYC_OP_DERIVE		1
#define CRYC_OP_KEYGEN		2
#define CRYC_OP_ENCAPSULATE	3
#define CRYC_OP_DECAPSULATE	4
#define CRYC_OP_VERIFY		5
#define CRYC_OP_FROMDATA	6

/*
 * An operation context: key (a reference, or NULL for a context from a
 * name), keygen_type the type such a context names, peer (a reference,
 * for derivation), operation once an _init function ran, and
 * signature_md the digest a verification's hash was made with (NULL when
 * none was set).
 */
struct evp_pkey_ctx_st {
	EVP_PKEY *key;
	int keygen_type;
	EVP_PKEY *peer;
	int operation;
	const EVP_MD *signature_md;
};

/* The KDF algorithm: only HKDF (the hash is chosen by a parameter). */
struct evp_kdf_st {
	int kind;
};

/* The largest info an HKDF context keeps (the joined info parameters). */
#define CRYC_KDF_MAX_INFO	32768U

/*
 * An HKDF context: hash (CRYC_HASH_NONE until a digest is set), the input
 * keying material key (malloc'd, key_set once given, possibly empty), the
 * salt and the info (malloc'd, NULL when empty).  The key is secret and
 * is wiped before it is freed; the salt and the info are wiped too.
 */
struct evp_kdf_ctx_st {
	enum cryc_hash_id hash;
	uint8_t *key;
	size_t key_size;
	int key_set;
	uint8_t *salt;
	size_t salt_size;
	uint8_t *info;
	size_t info_size;
};

/*
 * Makes a store with one reference (for SSL_CTX_new).  NULL without
 * memory (an error is queued).
 */
X509_STORE *cryc_x509_store_new(void);

/* Drops one reference to a store, freeing it at the last; NULL is ignored. */
void cryc_x509_store_free(X509_STORE *store);

/*
 * Makes a certificate object from a parsed peer certificate's DER (a
 * copy), with one reference: libssl-compat keeps the server's chain this
 * way.  NULL on failure.
 */
X509 *cryc_x509_from_der(const uint8_t *der, size_t size);

/* Maps a validation verdict to its X509_V_ code. */
long cryc_verify_code_to_x509(enum cryc_verify_code code);

/*
 * Compares a NUL-terminated name with an uppercase ASCII name, ignoring
 * the case of the letters of name: 1 when equal, 0 otherwise.
 */
int cryc_param_name_equal(const char *name, const char *expected);

/*
 * Compares a UTF-8 string parameter's value (data_size bytes, a last NUL
 * byte not counted) with an uppercase ASCII name, ignoring the case of
 * its letters: 1 when equal, 0 otherwise (also for another data type or a
 * NULL data with a nonzero size).
 */
int cryc_param_string_equal(const OSSL_PARAM *param, const char *expected);

/*
 * Tells whether a NUL-terminated group name is one of P-256's ("P-256",
 * "PRIME256V1", "SECP256R1", not case sensitive): 1 or 0.
 */
int cryc_param_p256_name(const char *name);

#endif
