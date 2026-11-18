/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libpasskey's verification of an assertion (ws161-p004; ws172 §7): a
 * pure function of bytes.  It touches no device and no file, so the
 * process that verifies (root's /sbin/passkey) needs nothing from the one
 * that talked to the key but the answer's bytes.
 *
 * The verifier gives what it expects: the relying party, the client data
 * hash it made, the flags it requires, and the credentials it allows (each
 * with the COSE key and the signature count it stored).  The answer names
 * a credential; its key is taken from the verifier's list, never from the
 * answer.  pk_verify_assertion() says which credential signed and gives
 * the count to store.
 */

#ifndef LIBPASSKEY_VERIFY_H
#define LIBPASSKEY_VERIFY_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include "crypto.h"

/* The authenticator data's flags (WebAuthn section 6.1). */
#define PK_FLAG_UP	0x01U
#define PK_FLAG_UV	0x04U
#define PK_FLAG_AT	0x40U
#define PK_FLAG_ED	0x80U

/* The shortest authenticator data: the relying party's hash, the flags and the count. */
#define PK_AUTH_DATA_MIN	37U

/* What pk_verify_assertion() answers besides 0 and EINVAL (errno values with these meanings). */
#define PK_VERIFY_NOT_ALLOWED	ENOENT		/* the credential is not one of the verifier's */
#define PK_VERIFY_MALFORMED	EBADMSG		/* the authenticator data or the stored key is not well formed */
#define PK_VERIFY_WRONG_PARTY	EXDEV		/* the relying party's hash is another's */
#define PK_VERIFY_FLAGS		EPERM		/* the user was not present, or not verified when required */
#define PK_VERIFY_SIGNATURE	EACCES		/* the signature does not verify */
#define PK_VERIFY_REPLAY	ESTALE		/* the signature count did not grow: the key may be a copy */

/* A credential the verifier allows: its ID, its stored COSE key, and its stored signature count. */
struct pk_credential {
	const uint8_t *id;
	size_t id_size;
	const uint8_t *cose_key;
	size_t cose_key_size;
	uint32_t sign_count;
};

/*
 * What the verifier expects: the relying party's ID, the client data hash
 * it made, the flags it requires (PK_FLAG_UP, PK_FLAG_UV), and the
 * credentials it allows.
 */
struct pk_expectation {
	const char *rp_id;
	uint8_t client_data_hash[PK_SHA256_SIZE];
	unsigned required_flags;
	const struct pk_credential *credentials;
	size_t credential_count;
};

/* The key's answer: the credential it used, the authenticator data, and the signature (DER). */
struct pk_assertion {
	const uint8_t *credential_id;
	size_t credential_id_size;
	const uint8_t *auth_data;
	size_t auth_data_size;
	const uint8_t *signature;
	size_t signature_size;
};

int pk_verify_assertion(const struct pk_expectation *expectation, const struct pk_assertion *assertion, size_t *matched, uint32_t *sign_count);
int pk_cose_p256(const uint8_t *cose_key, size_t size, uint8_t *x, uint8_t *y);
int pk_cose_ec2(const uint8_t *cose_key, size_t size, int64_t algorithm, uint8_t *x, uint8_t *y);

#endif
