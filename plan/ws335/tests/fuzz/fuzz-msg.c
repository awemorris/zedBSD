/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libFuzzer target: the parsers of the server's handshake messages
 * (sslc-msg.h).  The input is one message body.  FUZZ_MESSAGE, set when
 * this file is built, names the parser:
 *
 *   1 ServerHello (and HelloRetryRequest)   2 EncryptedExtensions
 *   3 Certificate (TLS 1.3 and TLS 1.2)     4 ServerKeyExchange
 *   5 the others: CertificateVerify, CertificateRequest (both versions),
 *     NewSessionTicket, KeyUpdate
 *
 * Every view the parser returns is read to its end, so that a view
 * reaching outside the input is caught.  Each body is a copy of exactly
 * its size.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "userland/base/libssl-compat/sslc-msg.h"

#ifndef FUZZ_MESSAGE
#error "FUZZ_MESSAGE must name the parser (1 to 5)"
#endif

/* The libFuzzer entry point, run once for every generated input. */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/* Reads every byte of a view, so that a view reaching outside the input is caught. */
static void touch(const uint8_t *view, size_t size);

/*
 * The sum of every byte read through a view.
 *
 * It is volatile so that the compiler cannot drop the reads, and it lives for the whole run.
 */
static volatile unsigned fuzz_sink;

/* Sums the bytes of a view, which reads every one of them. */
static void __attribute__((unused))
touch(
	const uint8_t *view,
	size_t size)
{
	size_t index;
	unsigned sum;

	/* Adds up every byte of the view. */
	sum = 0;
	for (index = 0; view != NULL && index < size; index++)
		sum += view[index];

	/* Keeps the sum alive so the reads are not dropped. */
	fuzz_sink += sum;
}

/*
 * Runs the parser chosen by FUZZ_MESSAGE on the input as one message body.
 *
 * Always reports 0, because a refusal is not a failure.
 */
int
LLVMFuzzerTestOneInput(
	const uint8_t *data,
	size_t size)
{
	uint8_t *body;
	size_t body_size;
	unsigned alert;
	int value;
	int status;
#if FUZZ_MESSAGE == 1
	struct sslc_server_hello hello;
#elif FUZZ_MESSAGE == 3
	struct sslc_certificate_msg certificates;
	size_t index;
	int tls13;
#elif FUZZ_MESSAGE == 4
	struct sslc_server_key_exchange exchange;
#elif FUZZ_MESSAGE == 5
	const uint8_t *view;
	size_t view_size;
	uint16_t scheme;
#endif

	/* Makes a copy of exactly size bytes, so a read past the end is caught. */
	body_size = size;
	if (body_size == 0U)
		body_size = 1U;
	body = malloc(body_size);
	if (body == NULL)
		return 0;

	/* Fills the copy with the input. */
	if (size != 0U)
		memcpy(body, data, size);

	/* Gives the parsers that take no value an output that is still used. */
	value = 0;
	(void)value;
#if FUZZ_MESSAGE == 1
	/* Parses a ServerHello and reads every view it returns. */
	status = sslc_msg_parse_server_hello(body, size, &hello, &alert);
	if (status == 0) {
		touch(hello.session_id, hello.session_id_size);
		touch(hello.share_key, hello.share_size);
		touch(hello.cookie, hello.cookie_size);
		touch(hello.renegotiation_info, hello.renegotiation_info_size);
	}
#elif FUZZ_MESSAGE == 2
	/* Parses an EncryptedExtensions. */
	status = sslc_msg_parse_encrypted_extensions(body, size, &value, &alert);
	(void)status;
#elif FUZZ_MESSAGE == 3
	/* Parses a Certificate in both protocol versions and reads every view it returns. */
	for (tls13 = 0; tls13 <= 1; tls13++) {
		status = sslc_msg_parse_certificate(body, size, tls13, &certificates, &alert);
		if (status != 0)
			continue;

		/* Reads the request context the message carried. */
		touch(certificates.context, certificates.context_size);

		/* Reads every certificate the message carried. */
		for (index = 0; index < certificates.count && index < CRYC_CHAIN_MAX_PEER; index++)
			touch(certificates.der[index], certificates.der_size[index]);
	}
#elif FUZZ_MESSAGE == 4
	/* Parses a ServerKeyExchange and reads every view it returns. */
	status = sslc_msg_parse_server_key_exchange(body, size, &exchange, &alert);
	if (status == 0) {
		touch(exchange.point, exchange.point_size);
		touch(exchange.signature, exchange.signature_size);
		touch(exchange.params, exchange.params_size);
	}
#elif FUZZ_MESSAGE == 5
	/* Parses a CertificateVerify and reads the signature it returns. */
	status = sslc_msg_parse_certificate_verify(body, size, &scheme, &view, &view_size, &alert);
	if (status == 0)
		touch(view, view_size);

	/* Parses a TLS 1.3 CertificateRequest and reads the view it returns. */
	status = sslc_msg_parse_certificate_request(body, size, 1, &view, &view_size, &alert);
	if (status == 0)
		touch(view, view_size);

	/* Parses a TLS 1.2 CertificateRequest and reads the view it returns. */
	status = sslc_msg_parse_certificate_request(body, size, 0, &view, &view_size, &alert);
	if (status == 0)
		touch(view, view_size);

	/* Parses the messages that return no view. */
	(void)sslc_msg_parse_new_session_ticket(body, size, &alert);
	(void)sslc_msg_parse_key_update(body, size, &value, &alert);
#endif

	/* Releases the copy. */
	free(body);

	/* The input never fails the run on its own. */
	return 0;
}
