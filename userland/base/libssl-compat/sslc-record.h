/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The protection of TLS records: the plaintext records before keys are
 * set, and the AEAD records of TLS 1.3 (RFC 8446 5.2 to 5.4) and of TLS
 * 1.2 (RFC 5246 6.2.3.3 with the nonces of RFC 5288 3 for AES-GCM and RFC
 * 7905 2 for ChaCha20-Poly1305).
 *
 * One struct sslc_record_dir is one direction of a connection (reading
 * or writing).  It is secret (keys and IVs) and is wiped by
 * sslc_record_clear and whenever new keys replace the old.
 *
 * Only AEAD ciphers exist here, so there is no MAC-then-encrypt and no
 * CBC padding: the padding-oracle class of attacks (Lucky Thirteen,
 * POODLE) has nothing to act on.  A record that fails its tag gives
 * EBADMSG and nothing else: no partial plaintext, and the same error
 * whatever the cause, so the caller sends bad_record_mac (5.2) and
 * closes.  The TLS 1.3 inner plaintext's padding is removed by scanning
 * from the end for the content type (5.4); its time depends on the
 * padding length, which the sender chose and which is not secret from
 * the sender (RFC 8446 5.4's note), and is bounded by the record size.
 */

#ifndef LIBSSL_COMPAT_SSLC_RECORD_H
#define LIBSSL_COMPAT_SSLC_RECORD_H

#include "../libcrypto-compat/cryc-chacha.h"
#include "../libcrypto-compat/cryc-gcm.h"
#include "sslc-tls.h"

/* The AEAD tag size of every suite (5.2: AES-GCM and ChaCha20-Poly1305 both 16). */
#define SSLC_RECORD_TAG		16U

/* The explicit nonce of a TLS 1.2 AES-GCM record (RFC 5288 3). */
#define SSLC_RECORD12_EXPLICIT	8U

/*
 * The keys of one direction, as the key schedule makes them (sslc-
 * keysched.h): the suite, the key (key_size bytes), and the IV (iv_size
 * bytes: 12 for TLS 1.3 and for TLS 1.2 ChaCha20-Poly1305, 4 for TLS 1.2
 * AES-GCM's salt).  Secret.
 */
struct sslc_traffic_keys {
	const struct sslc_suite *suite;
	uint8_t key[32];
	size_t key_size;
	uint8_t iv[12];
	size_t iv_size;
};

/*
 * One direction of a connection.
 *
 * suite is NULL before keys are set (records are plaintext: the
 * handshake's first messages and, in TLS 1.2, everything up to
 * ChangeCipherSpec).  version13 tells the TLS 1.3 record format from the
 * 1.2 one (fixed at the first key change).  gcm holds the expanded key for
 * the AES-GCM suites; chacha_key the key for ChaCha20-Poly1305.  iv and
 * iv_size as struct sslc_traffic_keys.  sequence is the record sequence
 * number (5.3; RFC 5246 6.1), reset to 0 by every key change.
 */
struct sslc_record_dir {
	const struct sslc_suite *suite;
	int version13;
	struct cryc_gcm gcm;
	uint8_t chacha_key[CRYC_CHACHA20_KEY];
	uint8_t iv[12];
	size_t iv_size;
	uint64_t sequence;
};

/*
 * A record read: its content type (the outer type for a plaintext or TLS
 * 1.2 record; the inner type for TLS 1.3, 5.4), and its plaintext (a view
 * into the buffer given to sslc_record_open, which decrypts in place).
 */
struct sslc_record {
	unsigned type;
	uint8_t *data;
	size_t size;
};

/* Makes a direction without keys (plaintext records, sequence 0). */
void sslc_record_init(struct sslc_record_dir *dir);

/*
 * Sets new keys (wiping the old) and resets the sequence number to 0.
 *
 * version13 is 1 for the TLS 1.3 record format.  keys->suite's AEAD and
 * the key and IV sizes must agree with the suite (EINVAL otherwise).
 * keys is not kept; the caller wipes it.  Returns 0 or EINVAL.
 */
int sslc_record_set_keys(struct sslc_record_dir *dir, int version13, const struct sslc_traffic_keys *keys);

/* Wipes the direction's keys and makes it plaintext again. */
void sslc_record_clear(struct sslc_record_dir *dir);

/*
 * Parses a record header (5.1; RFC 5246 6.2.1): type, legacy version,
 * length.
 *
 * header is 5 bytes.  version_check is the version the header must
 * carry, or 0 to accept 0x0301 to 0x0303 (the first records of a
 * connection, before the server's version is known: 5.1 allows 0x0301 in
 * a ServerHello's record).  Checks: a type among the four (unexpected
 * type: EPROTO, 5); a length of at least 1 and at most
 * SSLC_RECORD13_CIPHERTEXT_MAX for a protected TLS 1.3 direction,
 * SSLC_RECORD12_CIPHERTEXT_MAX for TLS 1.2, SSLC_RECORD_PLAINTEXT_MAX for
 * a plaintext direction (more: EMSGSIZE, the caller's record_overflow
 * alert); at least the protected minimum of the direction (TLS 1.3: 17,
 * the tag and the inner type; TLS 1.2 AES-GCM: 24, the explicit nonce
 * and the tag; TLS 1.2 ChaCha20-Poly1305: 16, the tag; shorter is
 * EBADMSG, the caller's bad_record_mac) and, on a plaintext direction, at
 * least 1 (a zero-length plaintext record is EPROTO, the caller's
 * decode_error; RFC 8446 5.1 and RFC 5246 6.2.1
 * forbid zero-length handshake and alert fragments, and a plaintext
 * direction carries only those).  The zero-length rule of TLS 1.2
 * application data is checked on the decrypted plaintext by
 * sslc_record_open's caller, not here.
 *
 * Returns 0, EPROTO or EMSGSIZE.
 */
int sslc_record_header(const struct sslc_record_dir *dir, const uint8_t *header, uint16_t version_check, unsigned *type, size_t *length);

/*
 * Returns the bytes of a whole protected record (header, nonce, payload,
 * tag) that carries size bytes of plaintext on this direction (for
 * TLS 1.3 one byte more for the inner content type, no padding).
 */
size_t sslc_record_overhead(const struct sslc_record_dir *dir, size_t size);

/*
 * Writes one record carrying size bytes of type (at most
 * SSLC_RECORD_PLAINTEXT_MAX) into out (room bytes, at least
 * sslc_record_overhead(dir, size)).
 *
 * Plaintext direction: header (type, version 0x0303, or 0x0301 for the
 * first ClientHello as 5.1 recommends when first_hello is 1) and the bytes.
 * TLS 1.3: header (application_data, 0x0303, length) and
 * AEAD-Encrypt(key, nonce, additional_data = header, TLSInnerPlaintext =
 * content || type) with nonce = iv xor the 64-bit sequence left-padded
 * (5.3), no padding added.  TLS 1.2 AES-GCM: header (type, 0x0303,
 * length), 8-byte explicit nonce = the sequence (RFC 5288 3 allows it and
 * it can never repeat), then ciphertext and tag with nonce = salt ||
 * explicit and additional_data = seq_num || type || version || length of
 * the plaintext (RFC 5246 6.2.3.3).  TLS 1.2 ChaCha20-Poly1305: as TLS 1.3's
 * nonce (RFC 7905 2) with the 1.2 additional data.
 *
 * in and out must not overlap.  *written receives the record's size.  The
 * sequence number advances on success; a sequence number that would wrap
 * (2^64 - 1 used) is EOVERFLOW (5.3: the connection must rekey or close;
 * libssl-compat closes).  Returns 0, ENOBUFS, EINVAL (size too large) or
 * EOVERFLOW.
 */
int sslc_record_seal(struct sslc_record_dir *dir, unsigned type, int first_hello, const uint8_t *in, size_t size, uint8_t *out, size_t room, size_t *written);

/*
 * Opens one whole record (its 5-byte header, already checked by
 * sslc_record_header, followed by length bytes) in place.
 *
 * Plaintext direction: the record is its plaintext.  TLS 1.3: the outer
 * type must be application_data, except a change_cipher_spec record
 * (5: a compatibility record, given back as type 20 unprotected for the
 * caller to check and drop, 5 and D.4) and an alert record before keys
 * are set; then the AEAD is opened, the zeros at the end of the inner
 * plaintext are removed, and the last non-zero byte is the inner type (an
 * inner plaintext of only zeros is EPROTO, 5.4; a TLSInnerPlaintext
 * (content, type and padding together) over 2^14 + 1 bytes is EMSGSIZE,
 * 5.4).  TLS 1.2: the explicit nonce for AES-GCM,
 * then AEAD open with the additional data of the plaintext length; a
 * plaintext over 2^14 bytes is EMSGSIZE.
 *
 * On success *out is the plaintext (a view into record) and the sequence
 * number advances.  A tag that does not verify is EBADMSG (the
 * plaintext bytes in record are zeroed).  Returns 0, EBADMSG, EPROTO,
 * EMSGSIZE or EOVERFLOW.
 */
int sslc_record_open(struct sslc_record_dir *dir, uint8_t *record, size_t size, struct sslc_record *out);

#endif
