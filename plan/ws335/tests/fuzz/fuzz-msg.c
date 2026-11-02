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

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/* Sums the bytes of a view, which reads every one of them. */
static volatile unsigned fuzz_sink;
static void __attribute__((unused))
touch(
	const uint8_t *view,
	size_t size)
{
	size_t index;
	unsigned sum;

	sum = 0;
	for (index = 0; view != NULL && index < size; index++)
		sum += view[index];
	fuzz_sink += sum;
}

int
LLVMFuzzerTestOneInput(
	const uint8_t *data,
	size_t size)
{
	uint8_t *body;
	unsigned alert;
	int value;
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

	body = malloc(size != 0U ? size : 1U);
	if (body == NULL)
		return 0;
	if (size != 0U)
		memcpy(body, data, size);
	value = 0;
	(void)value;
#if FUZZ_MESSAGE == 1
	if (sslc_msg_parse_server_hello(body, size, &hello, &alert) == 0) {
		touch(hello.session_id, hello.session_id_size);
		touch(hello.share_key, hello.share_size);
		touch(hello.cookie, hello.cookie_size);
		touch(hello.renegotiation_info, hello.renegotiation_info_size);
	}
#elif FUZZ_MESSAGE == 2
	(void)sslc_msg_parse_encrypted_extensions(body, size, &value, &alert);
#elif FUZZ_MESSAGE == 3
	for (tls13 = 0; tls13 <= 1; tls13++) {
		if (sslc_msg_parse_certificate(body, size, tls13, &certificates, &alert) != 0)
			continue;
		touch(certificates.context, certificates.context_size);
		for (index = 0; index < certificates.count && index < CRYC_CHAIN_MAX_PEER; index++)
			touch(certificates.der[index], certificates.der_size[index]);
	}
#elif FUZZ_MESSAGE == 4
	if (sslc_msg_parse_server_key_exchange(body, size, &exchange, &alert) == 0) {
		touch(exchange.point, exchange.point_size);
		touch(exchange.signature, exchange.signature_size);
		touch(exchange.params, exchange.params_size);
	}
#elif FUZZ_MESSAGE == 5
	if (sslc_msg_parse_certificate_verify(body, size, &scheme, &view, &view_size, &alert) == 0)
		touch(view, view_size);
	if (sslc_msg_parse_certificate_request(body, size, 1, &view, &view_size, &alert) == 0)
		touch(view, view_size);
	if (sslc_msg_parse_certificate_request(body, size, 0, &view, &view_size, &alert) == 0)
		touch(view, view_size);
	(void)sslc_msg_parse_new_session_ticket(body, size, &alert);
	(void)sslc_msg_parse_key_update(body, size, &value, &alert);
#endif
	free(body);
	return 0;
}
