/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the test tools themselves: hex_decode, and that the record
 * files of the fetched vectors read back with their documented counts.
 * It also links every module of both libraries, so a missing definition
 * shows here first.
 */

#include "check.h"
#include "derwriter.h"
#include "vecfile.h"

#include "userland/base/libssl-compat/sslc-api.h"

/* Counts the records of one vector file; returns the count, or 0 when it cannot be opened. */
static unsigned long
count_records(
	const char *name)
{
	struct vec_file file;
	unsigned long count;

	/* Every record. */
	if (vec_open(&file, name) != 0)
		return 0;
	count = 0;
	while (vec_next(&file))
		count++;
	vec_close(&file);
	return count;
}

/* Runs the checks. */
int
main(void)
{
	static const unsigned char ec_public_key_oid[] = { 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01 };
	struct der_writer writer;
	struct vec_file file;
	unsigned char bytes[64];
	size_t size;
	int found;

	/* hex_decode: good, odd, bad character, no room. */
	CHECK(hex_decode("00ff7a", bytes, sizeof(bytes)) == 3U);
	CHECK(bytes[0] == 0x00 && bytes[1] == 0xff && bytes[2] == 0x7a);
	CHECK(hex_decode("abc", bytes, sizeof(bytes)) == (size_t)-1);
	CHECK(hex_decode("zz", bytes, sizeof(bytes)) == (size_t)-1);
	CHECK(hex_decode("0011", bytes, 1) == (size_t)-1);
	CHECK(hex_decode("", bytes, sizeof(bytes)) == 0U);

	/* derwriter: id-ecPublicKey (the arcs of RFC 5480 2.1.1, encoded by hand from X.690 8.19), a long-form length. */
	der_init(&writer, bytes, sizeof(bytes));
	der_oid(&writer, "1.2.840.10045.2.1");
	CHECK(!writer.error && writer.used == sizeof(ec_public_key_oid));
	CHECK_BYTES(bytes, ec_public_key_oid, sizeof(ec_public_key_oid));
	der_init(&writer, bytes, sizeof(bytes));
	der_length(&writer, 300U);
	CHECK(writer.used == 3U && bytes[0] == 0x82 && bytes[1] == 0x01 && bytes[2] == 0x2c);

	/* The record counts of a few files (RFC 8448: 538 labelled values; limbo: its SERVER cases). */
	CHECK(count_records("rfc8448") == 538UL);
	CHECK(count_records("limbo") == 9792UL);
	CHECK(count_records("wycheproof-x25519_test") > 0UL);
	CHECK(count_records("cavp-gcmtestvectors-gcmEncryptExtIV128") > 0UL);

	/* The first x25519 key of RFC 8448 section 3 reads back. */
	found = 0;
	if (vec_open(&file, "rfc8448") == 0) {
		while (!found && vec_next(&file)) {
			if (strcmp(vec_get(&file, "label"), "private key") == 0) {
				size = vec_hex(&file, "hex", bytes, sizeof(bytes));
				CHECK(size == 32U);
				CHECK(bytes[0] == 0x49 && bytes[31] == 0x05);
				found = 1;
			}
		}
		vec_close(&file);
	}
	CHECK(found);

	/* The libraries link (a function of each end). */
	CHECK(cryc_hash_size(CRYC_HASH_SHA256) == 0U || cryc_hash_size(CRYC_HASH_SHA256) == 32U);
	CHECK(TLS_client_method() == NULL || TLS_client_method() != NULL);

	return check_finish("test-tools");
}
