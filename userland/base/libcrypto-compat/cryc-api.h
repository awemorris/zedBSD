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

#include <compat/crypto/crypto.h>
#include <compat/crypto/err.h>
#include <compat/crypto/evp.h>
#include <compat/crypto/rand.h>
#include <compat/crypto/x509.h>

#include "cryc-chain.h"
#include "cryc-chacha.h"
#include "cryc-ed25519.h"
#include "cryc-err.h"
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

/* The cipher algorithm: only ChaCha20. */
struct evp_cipher_st {
	int key_size;
	int iv_size;
};

/*
 * A ChaCha20 context: key (key_set when given), the 64-bit block
 * counter and the 8-byte nonce from the IV, and the unused rest of the
 * current keystream block (keystream[64 - available .. 63]).  Secret.
 */
struct evp_cipher_ctx_st {
	const EVP_CIPHER *cipher;
	uint8_t key[CRYC_CHACHA20_KEY];
	int key_set;
	uint64_t counter;
	uint8_t nonce[8];
	uint8_t keystream[CRYC_CHACHA20_BLOCK];
	size_t available;
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

/*
 * A key.  public_key holds the raw public form (32 bytes, or 1184 for
 * ML-KEM-768); private_key, when has_private, is a malloc'd raw private
 * form (32 bytes: Ed25519's seed or X25519's scalar; 2400 bytes:
 * ML-KEM-768's dk), wiped before it is freed.
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

/*
 * An operation context: key (a reference, or NULL for a keygen context
 * from a name), keygen_type for such a context, peer (a reference, for
 * derivation), operation once an _init function ran.
 */
struct evp_pkey_ctx_st {
	EVP_PKEY *key;
	int keygen_type;
	EVP_PKEY *peer;
	int operation;
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

#endif
