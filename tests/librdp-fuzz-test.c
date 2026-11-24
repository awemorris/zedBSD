/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Parser robustness and ASan/UBSan fuzz harness for userland/desktop/librdp.
 */

#include "userland/desktop/librdp/rdp-internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int discard_send(void *user, const uint8_t *data, size_t len);
static void fuzz_one_input(const uint8_t *data, size_t len);

/*
 * Exercises all librdp decoders against mutated and malformed inputs.
 */
int
main(
	void)
{
	uint8_t buf[512];
	uint32_t rng;
	size_t iter;
	size_t len;
	size_t i;

	/* Deterministic PRNG fuzzing across 20,000 malformed/mutated inputs. */
	rng = 0x12345678U;
	for (iter = 0U; iter < 20000U; iter++) {
		rng = rng * 1664525U + 1013904223U;
		len = (size_t)(rng % sizeof(buf));
		for (i = 0U; i < len; i++) {
			rng = rng * 1664525U + 1013904223U;
			buf[i] = (uint8_t)(rng >> 24);
		}

		/* Forces TPKT or Fast-Path prefix half the time to reach deep parsers. */
		if (len >= 4U && (iter & 1U) == 0U) {
			buf[0] = 0x03U;
			buf[1] = 0x00U;
			buf[2] = (uint8_t)((len >> 8) & 0xFFU);
			buf[3] = (uint8_t)(len & 0xFFU);
		}

		fuzz_one_input(buf, len);
	}

	printf("librdp-fuzz-test: 20000 iterations passed without crash\n");

	/* Succeeded: fuzz harness completed cleanly. */
	return 0;
}

/* Discards outgoing bytes during fuzzing. */
static int
discard_send(
	void *user,
	const uint8_t *data,
	size_t len)
{
	UNUSED_PARAMETER(user);
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(len);
	return 0;
}

/* Feeds one arbitrary byte buffer into all librdp entry points. */
static void
fuzz_one_input(
	const uint8_t *data,
	size_t len)
{
	struct rdp_callbacks cb;
	struct rdp_client *c;
	struct rdp_server *s;
	struct rdp_client_config ccfg;
	struct rdp_x224_neg neg;
	const uint8_t *dt;
	size_t dt_len;
	struct rdp_mcs_connect_info mcs_ci;
	struct rdp_mcs_pdu mcs_pdu;
	struct rdp_sec_info sec_info;
	struct rdp_active_pdu act;
	struct rdp_dynvc_msg dyn_msg;
	struct rdp_avc_stream avc;
	struct rdp_avc_rect lrects[16];
	struct rdp_avc_quant lquants[16];
	struct rdp_avc_rect crects[16];
	struct rdp_avc_quant cquants[16];
	uint16_t codec;
	uint32_t w;
	uint32_t h;

	memset(&cb, 0, sizeof(cb));
	cb.send_bytes = discard_send;

	/* Fuzzes client state machine in initial and post-TLS states. */
	c = rdp_client_create(&cb, NULL);
	if (c != NULL) {
		memset(&ccfg, 0, sizeof(ccfg));
		rdp_client_connect(c, &ccfg);
		rdp_client_feed(c, data, len);
		rdp_client_destroy(c);
	}

	/* Fuzzes server state machine from X.224 and from post-TLS. */
	s = rdp_server_create(&cb, NULL, NULL);
	if (s != NULL) {
		rdp_server_feed(s, data, len);
		rdp_server_destroy(s);
	}

	/* Fuzzes individual wire decoders directly. */
	rdp_x224_decode(data, len, &neg, &dt, &dt_len);
	rdp_mcs_decode_connect_initial(data, len, &mcs_ci);
	rdp_mcs_decode_connect_response(data, len, &mcs_ci);
	rdp_mcs_decode_pdu(data, len, &mcs_pdu);
	rdp_sec_decode_info_pkt(data, len, &sec_info);
	rdp_sec_decode_license_valid(data, len);
	rdp_sec_decode_active_pdu(data, len, RDP_PDUTYPE_DEMANDACTIVE, &act);
	rdp_dynvc_decode(data, len, &dyn_msg);
	rdp_gfx_decode_avc_pdu(data, len, &codec, &avc, lrects, lquants,
			       crects, cquants, 16U);
	rdp_disp_decode_monitor_layout(data, len, &w, &h);
	rdp_input_decode_events(data, len, 4U, &cb, NULL);
}
