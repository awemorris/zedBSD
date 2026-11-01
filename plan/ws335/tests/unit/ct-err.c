/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The constant-time checks of the secret-handling calls of libcrypto-
 * compat's error module: CRYPTO_memcmp on secret bytes (equal and
 * different, the difference at the first, a middle and the last byte) and
 * OPENSSL_cleanse of a secret.  Only the answer of the comparison is
 * public.
 */

#include "check.h"
#include "ct.h"

#include <compat/crypto/crypto.h>

/* Runs the checks. */
int
main(void)
{
	unsigned char left[96];
	unsigned char right[96];
	unsigned char secret[64];
	size_t positions[3];
	size_t index;
	size_t which;
	int result;

	/* The three places a difference is put. */
	positions[0] = 0;
	positions[1] = 50U;
	positions[2] = sizeof(left) - 1U;

	/* Equal secrets compare equal. */
	for (index = 0; index < sizeof(left); index++) {
		left[index] = (unsigned char)(index * 11U + 3U);
		right[index] = left[index];
	}
	CT_SECRET(left, sizeof(left));
	CT_SECRET(right, sizeof(right));
	result = CRYPTO_memcmp(left, right, sizeof(left));
	CT_PUBLIC(&result, sizeof(result));
	CHECK(result == 0);
	CT_PUBLIC(left, sizeof(left));
	CT_PUBLIC(right, sizeof(right));

	/* A difference at each place is found, with the secrets marked each time. */
	for (which = 0; which < 3U; which++) {
		right[positions[which]] = (unsigned char)(right[positions[which]] ^ 0x80U);
		CT_SECRET(left, sizeof(left));
		CT_SECRET(right, sizeof(right));
		result = CRYPTO_memcmp(left, right, sizeof(left));
		CT_PUBLIC(&result, sizeof(result));
		CHECK(result != 0);
		CT_PUBLIC(left, sizeof(left));
		CT_PUBLIC(right, sizeof(right));
		right[positions[which]] = left[positions[which]];
	}

	/* Wiping a secret needs no branch or address that depends on it. */
	for (index = 0; index < sizeof(secret); index++)
		secret[index] = (unsigned char)(index + 1U);
	CT_SECRET(secret, sizeof(secret));
	OPENSSL_cleanse(secret, sizeof(secret));
	CT_PUBLIC(secret, sizeof(secret));
	CHECK(secret[0] == 0 && secret[63] == 0);

	/* The last line run-ct.sh reads. */
	return check_finish("ct-err");
}
