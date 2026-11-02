/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libFuzzer target: certificates.  The input is parsed with
 * cryc_x509_parse (and, when it parses, its key is read again and its
 * names are matched against a host, an address, and its own name
 * constraints as a CA over itself), and with the public d2i_X509.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <compat/crypto/x509.h>

#include "userland/base/libcrypto-compat/cryc-name.h"
#include "userland/base/libcrypto-compat/cryc-x509.h"

/* The libFuzzer entry point, run once for every generated input. */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/*
 * Parses the input as a certificate through both parsers.
 *
 * Always reports 0, because a refusal is not a failure.
 */
int
LLVMFuzzerTestOneInput(
	const uint8_t *data,
	size_t size)
{
	struct cryc_x509 certificate;
	struct cryc_pubkey key;
	static const uint8_t address[4] = { 192, 0, 2, 1 };
	const unsigned char *cursor;
	uint8_t *copy;
	size_t copy_size;
	int status;
	X509 *public_certificate;

	/* A copy of exactly size bytes, so a read past the end is caught. */
	copy_size = size;
	if (copy_size == 0U)
		copy_size = 1U;
	copy = malloc(copy_size);
	if (copy == NULL)
		return 0;

	/* Fills the copy with the input. */
	if (size != 0U)
		memcpy(copy, data, size);

	/* Parses the input directly, then reads what a parsed certificate offers. */
	status = cryc_x509_parse(&certificate, copy, size);
	if (status == 0) {
		(void)cryc_x509_public_key(certificate.spki, certificate.spki_size, &key);
		(void)cryc_name_match_host(&certificate, "www.example.com");
		(void)cryc_name_match_ip(&certificate, address, sizeof(address));
		(void)cryc_name_constraints_check(&certificate, &certificate, 1);
	}

	/* The public parser. */
	cursor = copy;
	public_certificate = d2i_X509(NULL, &cursor, (long)size);
	X509_free(public_certificate);
	free(copy);

	/* The input never fails the run on its own. */
	return 0;
}
