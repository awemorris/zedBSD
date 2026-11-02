/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libcrypto-compat's EVP interface: the subset of OpenSSL 3.5's that
 * zedBSD's ssh and sshd and the security key code (libpasskey) use.
 * Names, signatures and return conventions are OpenSSL 3.5's (functions
 * return 1 on success and 0 or a negative value on failure, queuing an
 * error, unless a comment says otherwise); the behaviour described is the
 * subset's, documented here.
 *
 * Algorithms: SHA-256 (EVP_sha256), ChaCha20 (EVP_chacha20), AES-256 in
 * CBC mode without padding (EVP_aes_256_cbc), Poly1305 (EVP_MAC
 * "POLY1305"), Ed25519 (EVP_PKEY_ED25519) for signatures, X25519
 * (EVP_PKEY_X25519) for key agreement, ML-KEM-768 ("ML-KEM-768") for key
 * encapsulation, and EC keys on P-256 ("EC" with the group "prime256v1")
 * for ECDSA verification with SHA-256 and ECDH.  HMAC (<compat/crypto/
 * hmac.h>) and HKDF (<compat/crypto/kdf.h>) are beside.  Anything else
 * (another digest, cipher, key type or operation) fails.  Every ENGINE,
 * OSSL_LIB_CTX and property query argument must be NULL, and so must
 * every OSSL_PARAM array argument except those of EVP_PKEY_fromdata.
 *
 * A failure queues an error of the EVP library whose reason is OpenSSL's
 * number for the condition (a NULL argument, an argument that must be
 * NULL or is out of range, an unsupported algorithm or key type, a wrong
 * key length, an unusable key, an operation not prepared, a buffer too
 * small, keys of different types); a success queues nothing.  A failing
 * EVP_PKEY function leaves the keys, the context and the caller's buffers
 * and lengths as they were, unless its comment says otherwise.  Key type
 * and algorithm names are not case sensitive.
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

/*
 * The parts of a key EVP_PKEY_fromdata is asked to take (the selection).
 * Values referenced: OpenSSL 3.5.8 include/openssl/core_dispatch.h
 * (OSSL_KEYMGMT_SELECT_PRIVATE_KEY, _PUBLIC_KEY, _DOMAIN_PARAMETERS,
 * _OTHER_PARAMETERS) via include/openssl/evp.h (EVP_PKEY_KEY_PARAMETERS,
 * EVP_PKEY_PRIVATE_KEY, EVP_PKEY_PUBLIC_KEY, EVP_PKEY_KEYPAIR).
 */
#define OSSL_KEYMGMT_SELECT_PRIVATE_KEY		0x01
#define OSSL_KEYMGMT_SELECT_PUBLIC_KEY		0x02
#define OSSL_KEYMGMT_SELECT_DOMAIN_PARAMETERS	0x04
#define OSSL_KEYMGMT_SELECT_OTHER_PARAMETERS	0x80
#define EVP_PKEY_KEY_PARAMETERS	(OSSL_KEYMGMT_SELECT_DOMAIN_PARAMETERS | OSSL_KEYMGMT_SELECT_OTHER_PARAMETERS)
#define EVP_PKEY_PRIVATE_KEY	(EVP_PKEY_KEY_PARAMETERS | OSSL_KEYMGMT_SELECT_PRIVATE_KEY)
#define EVP_PKEY_PUBLIC_KEY	(EVP_PKEY_KEY_PARAMETERS | OSSL_KEYMGMT_SELECT_PUBLIC_KEY)
#define EVP_PKEY_KEYPAIR	(EVP_PKEY_PUBLIC_KEY | OSSL_KEYMGMT_SELECT_PRIVATE_KEY)

/* OpenSSL 3's older name of EVP_MD_get_size. */
#define EVP_MD_size		EVP_MD_get_size

/* Returns the SHA-256 digest algorithm (a constant object). */
const EVP_MD *EVP_sha256(void);

/* Returns a digest's output size in bytes (32 for SHA-256), or -1 for NULL (nothing queued). */
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

/*
 * Makes out a copy of the digest in progress in in (a snapshot); out need
 * not be started.  in must hold a digest started with EVP_DigestInit_ex
 * and not finished (a finished one, or a context of a signature or
 * verification, fails).
 */
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
 * the size on return.  An EC key has no raw form (0; use
 * EVP_PKEY_get_octet_string_param).
 */
int EVP_PKEY_get_raw_public_key(const EVP_PKEY *pkey, unsigned char *key, size_t *length);

/*
 * Writes a key's raw private form (32 bytes: Ed25519's seed, X25519's
 * scalar), as EVP_PKEY_get_raw_public_key; 0 for a public-only key, for
 * ML-KEM-768 (its private form is not exported) and for an EC key.
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
 * Signs tbs (one shot, RFC 8032 5.1.6).  ctx must have been started with
 * EVP_DigestSignInit, also for the size: with signature NULL sets *length
 * to 64 and returns 1; otherwise *length is the room (at least 64) and
 * becomes 64.
 */
int EVP_DigestSign(EVP_MD_CTX *ctx, unsigned char *signature, size_t *length, const unsigned char *tbs, size_t tbs_length);

/* Starts an Ed25519 verification with pkey (public key enough); arguments as EVP_DigestSignInit. */
int EVP_DigestVerifyInit(EVP_MD_CTX *ctx, EVP_PKEY_CTX **pctx, const EVP_MD *type, ENGINE *e, EVP_PKEY *pkey);

/*
 * Verifies a signature over tbs (one shot, RFC 8032 5.1.7).  Returns 1
 * when it verifies; 0 when it does not, including a signature of a length
 * other than 64 (nothing queued: a verdict, not an error); 0 with an error
 * queued for a NULL signature or a NULL tbs that is not empty; -1 with an
 * error queued for a NULL context or one not started for verification.
 */
int EVP_DigestVerify(EVP_MD_CTX *ctx, const unsigned char *signature, size_t length, const unsigned char *tbs, size_t tbs_length);

/* Makes an operation context on pkey (taking a reference); e must be NULL.  NULL on failure. */
EVP_PKEY_CTX *EVP_PKEY_CTX_new(EVP_PKEY *pkey, ENGINE *e);

/*
 * Makes a context for a key type by name (libctx and propquery NULL):
 * "ML-KEM-768" for EVP_PKEY_keygen, "EC" for EVP_PKEY_fromdata (not case
 * sensitive).  NULL otherwise.
 */
EVP_PKEY_CTX *EVP_PKEY_CTX_new_from_name(OSSL_LIB_CTX *libctx, const char *name, const char *propquery);

/* As EVP_PKEY_CTX_new (libctx and propquery NULL). */
EVP_PKEY_CTX *EVP_PKEY_CTX_new_from_pkey(OSSL_LIB_CTX *libctx, EVP_PKEY *pkey, const char *propquery);

/* Frees a context and drops its keys; NULL is ignored. */
void EVP_PKEY_CTX_free(EVP_PKEY_CTX *ctx);

/* Prepares an X25519 or P-256 ECDH derivation with the context's private key; a peer set before is let go. */
int EVP_PKEY_derive_init(EVP_PKEY_CTX *ctx);

/* Sets the peer's public key, of the same type as the context's key (the context takes a reference). */
int EVP_PKEY_derive_set_peer(EVP_PKEY_CTX *ctx, EVP_PKEY *peer);

/*
 * Derives the shared secret: X25519's (RFC 7748 6.1), or P-256 ECDH's,
 * the x coordinate of the shared point (NIST SP 800-56A Rev. 3 5.7.1.2),
 * 32 bytes either way.  With key NULL sets *length to 32 and returns 1.
 * An all-zero X25519 result fails (returns 0, nothing usable written: the
 * 32 bytes of key are zeroed, *length is unchanged).
 */
int EVP_PKEY_derive(EVP_PKEY_CTX *ctx, unsigned char *key, size_t *length);

/* Prepares a key generation on a context from EVP_PKEY_CTX_new_from_name("ML-KEM-768") (an "EC" context: 0). */
int EVP_PKEY_keygen_init(EVP_PKEY_CTX *ctx);

/*
 * Generates an ML-KEM-768 key pair (FIPS 203 Algorithm 19) into a new key
 * in *pkey, with one reference.  A key *pkey pointed to before is not
 * freed: the caller keeps its reference.
 */
int EVP_PKEY_keygen(EVP_PKEY_CTX *ctx, EVP_PKEY **pkey);

/* Prepares an encapsulation to the context's ML-KEM-768 public key; params must be NULL. */
int EVP_PKEY_encapsulate_init(EVP_PKEY_CTX *ctx, const OSSL_PARAM params[]);

/*
 * Encapsulates (FIPS 203 Algorithm 20): writes the 1088-byte ciphertext
 * to wrapped and the 32-byte shared key to key.  Both lengths are
 * required.  With both buffers NULL sets the two lengths and returns 1;
 * one buffer without the other fails; otherwise each length is the room
 * (too small: 0, before any random bytes are drawn) and becomes the size.
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

/*
 * Generates a key pair in one call (libctx and propq NULL): type "EC"
 * followed by one more argument, the group's name as a const char *
 * ("P-256", "prime256v1" or "secp256r1"; not case sensitive), gives a
 * P-256 key pair (FIPS 186-5 A.2.2).  Any other type or group: NULL.
 */
EVP_PKEY *EVP_PKEY_Q_keygen(OSSL_LIB_CTX *libctx, const char *propq, const char *type, ...);

/* Prepares a key made from parameters on a context from EVP_PKEY_CTX_new_from_name("EC"); -2 for another context. */
int EVP_PKEY_fromdata_init(EVP_PKEY_CTX *ctx);

/*
 * Makes a P-256 public key from params into *ppkey, which must be NULL on
 * entry (a new key with one reference is stored there).  selection must
 * include OSSL_KEYMGMT_SELECT_PUBLIC_KEY (EVP_PKEY_PUBLIC_KEY or
 * EVP_PKEY_KEYPAIR).  params holds exactly OSSL_PKEY_PARAM_GROUP_NAME, a
 * UTF-8 string "prime256v1" (or "P-256", "secp256r1"; not case
 * sensitive), and OSSL_PKEY_PARAM_PUB_KEY, an octet string, the
 * uncompressed point (65 bytes, SEC 1 2.3.3), which must be on the curve.
 * Any other parameter (a private key among them), a compressed point or a
 * point off the curve fails (0, *ppkey unchanged).
 */
int EVP_PKEY_fromdata(EVP_PKEY_CTX *ctx, EVP_PKEY **ppkey, int selection, OSSL_PARAM params[]);

/*
 * Checks the public key of the context's key: for an EC key, that the
 * point is on the curve (SP 800-56A Rev. 3 5.6.2.3.3), 1 or 0.  -2 for the
 * other key types.
 */
int EVP_PKEY_public_check(EVP_PKEY_CTX *ctx);

/*
 * Writes the octet string parameter key_name of pkey into buf:
 * OSSL_PKEY_PARAM_PUB_KEY or OSSL_PKEY_PARAM_ENCODED_PUBLIC_KEY, the
 * public key (an EC key's uncompressed point of 65 bytes; the raw public
 * form for the other types).  With buf NULL sets *out_len to the size and
 * returns 1; otherwise max_buf_sz is the room (too small: 0) and *out_len,
 * when out_len is not NULL, becomes the size.  Another name: 0.
 */
int EVP_PKEY_get_octet_string_param(const EVP_PKEY *pkey, const char *key_name, unsigned char *buf, size_t max_buf_sz, size_t *out_len);

/* Prepares an ECDSA verification with the context's EC key (public enough); -2 for another key type. */
int EVP_PKEY_verify_init(EVP_PKEY_CTX *ctx);

/*
 * Sets the digest the verified hash was made with (EVP_sha256 only) on a
 * context prepared with EVP_PKEY_verify_init: EVP_PKEY_verify then takes
 * only a 32-byte hash.  -2 for a context not prepared for verification.
 */
int EVP_PKEY_CTX_set_signature_md(EVP_PKEY_CTX *ctx, const EVP_MD *md);

/*
 * Verifies an ECDSA signature (FIPS 186-5 6.4.2) in its DER form
 * (Ecdsa-Sig-Value, RFC 3279 2.2.3) over tbs, the hash of the message
 * (any size of 1 or more bytes without a digest set: its leftmost 256
 * bits are used; 32 with one).  Returns 1 when it verifies; 0 when it does
 * not or when tbs has the wrong size for the digest set (nothing queued);
 * -1 with an error queued for a context not prepared, a NULL argument, or
 * a signature that is not strict DER or whose r or s is zero, negative or
 * longer than 32 bytes (OpenSSL gives 0 for the last three).
 */
int EVP_PKEY_verify(EVP_PKEY_CTX *ctx, const unsigned char *sig, size_t siglen, const unsigned char *tbs, size_t tbslen);

/* Returns the ChaCha20 cipher (a constant object): 32-byte key, 16-byte IV. */
const EVP_CIPHER *EVP_chacha20(void);

/*
 * Returns the AES-256-CBC cipher (a constant object): 32-byte key, 16-byte
 * IV, 16-byte blocks (FIPS 197, NIST SP 800-38A 6.2).  Only the form
 * without padding is supported: a program turns padding off with
 * EVP_CIPHER_CTX_set_padding(ctx, 0) after EVP_CipherInit_ex and before
 * any data; with padding on, EVP_CipherUpdate and EVP_CipherFinal_ex fail.
 */
const EVP_CIPHER *EVP_aes_256_cbc(void);

/* Makes an empty cipher context; NULL without memory. */
EVP_CIPHER_CTX *EVP_CIPHER_CTX_new(void);

/* Wipes and frees a cipher context; NULL is ignored. */
void EVP_CIPHER_CTX_free(EVP_CIPHER_CTX *ctx);

/*
 * Sets up ChaCha20: cipher EVP_chacha20 (or NULL to keep the one set),
 * key 32 bytes (or NULL to keep it), iv 16 bytes (or NULL to keep it),
 * enc 1, 0 or -1 (keep; ChaCha20 is the same both ways).  The IV is
 * OpenSSL's layout: bytes 0 to 15 are the block function's state words
 * 12 to 15, little-endian, and the block counter is words 12 and 13 taken
 * as one 64-bit little-endian counter.  A new context has no key and a
 * zero IV; a key alone keeps the IV (and drops the buffered
 * keystream), an IV alone keeps the key, and setting an IV restarts the
 * keystream.  A call that would need a block past counter 2^64 - 1 fails
 * before anything is written, and once the block with counter 2^64 - 1
 * has been made only its buffered bytes remain; a new IV or key goes on.
 *
 * Sets up AES-256-CBC: cipher EVP_aes_256_cbc starts afresh (no key, a
 * zero IV, padding on, no buffered bytes) and NULL keeps the AES set up;
 * key 32 bytes, iv 16 bytes (each NULL to keep it); enc 1 encrypts, 0
 * decrypts, -1 keeps the direction.  A key or an IV drops any buffered
 * partial block.
 */
int EVP_CipherInit_ex(EVP_CIPHER_CTX *ctx, const EVP_CIPHER *cipher, ENGINE *impl, const unsigned char *key, const unsigned char *iv, int enc);

/*
 * ChaCha20: XORs in_length bytes of in with the keystream into out (out
 * may be in), continuing the keystream across calls; *out_length becomes
 * in_length.
 *
 * AES-256-CBC (padding off): takes in_length bytes after the partial
 * block buffered by earlier calls, encrypts or decrypts every whole block
 * they make into out (the room must be in_length + 15 bytes), keeps the
 * rest (under 16 bytes) for the next call, and sets *out_length to the
 * bytes written.  out may be in only while no partial block is buffered
 * (whole-block calls); otherwise they must not overlap.  in_length must be
 * at most INT_MAX - 16.
 */
int EVP_CipherUpdate(EVP_CIPHER_CTX *ctx, unsigned char *out, int *out_length, const unsigned char *in, int in_length);

/*
 * Ends a cipher: writes nothing and sets *out_length to 0.  AES-256-CBC
 * without padding fails (0) when a partial block is buffered, and with
 * padding on.  The context needs EVP_CipherInit_ex before more data.
 */
int EVP_CipherFinal_ex(EVP_CIPHER_CTX *ctx, unsigned char *out, int *out_length);

/*
 * Turns the block cipher's padding on (pad not 0, the default after
 * EVP_CipherInit_ex with a cipher) or off (0).  Returns 1 (0 for a NULL
 * context).  ChaCha20 ignores it.
 */
int EVP_CIPHER_CTX_set_padding(EVP_CIPHER_CTX *ctx, int pad);

/*
 * As EVP_CipherUpdate without the length output, for ChaCha20 only.
 * Returns length (as int) on success, and -1 on failure (a context not
 * set up or not ChaCha20, length over INT_MAX), as OpenSSL 3.5 does for
 * provider ciphers.
 */
int EVP_Cipher(EVP_CIPHER_CTX *ctx, unsigned char *out, const unsigned char *in, unsigned int length);

/* Returns the MAC algorithm named "POLY1305" (libctx and properties NULL), or NULL for another name. */
EVP_MAC *EVP_MAC_fetch(OSSL_LIB_CTX *libctx, const char *algorithm, const char *properties);

/* Releases a MAC algorithm; NULL is ignored. */
void EVP_MAC_free(EVP_MAC *mac);

/* Makes a MAC context for mac; NULL on failure. */
EVP_MAC_CTX *EVP_MAC_CTX_new(EVP_MAC *mac);

/* Wipes and frees a MAC context; NULL is ignored. */
void EVP_MAC_CTX_free(EVP_MAC_CTX *ctx);

/*
 * (Re)starts Poly1305 with a 32-byte one-time key (params NULL); may be
 * called again on the same context.  The key is required every time
 * (Poly1305 keeps no key between computations).
 */
int EVP_MAC_init(EVP_MAC_CTX *ctx, const unsigned char *key, size_t length, const OSSL_PARAM params[]);

/* Absorbs length bytes. */
int EVP_MAC_update(EVP_MAC_CTX *ctx, const unsigned char *data, size_t length);

/*
 * Writes the 16-byte tag (out_size at least 16; *out_length 16 when
 * out_length is not NULL); the context needs EVP_MAC_init before reuse.
 * With out NULL sets *out_length to 16 and returns 1, computing nothing.
 */
int EVP_MAC_final(EVP_MAC_CTX *ctx, unsigned char *out, size_t *out_length, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif
