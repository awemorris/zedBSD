/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of bluetoothd's pairing parts (ws143-p004,
 * plan/ws143/phase004/phase.md section 9): crypto.c, acl.c, l2cap.c,
 * smp.c and keys.c built with the host's compiler.
 *
 *   crypto   FIPS-197's example, RFC 4493's four examples, and the Core's
 *            sample data (Vol 3 Part H Appendix D) for ah, c1, s1, f4, f5,
 *            f6 and g2 (each checked with python's cryptography 43 when
 *            the test was written)
 *   acl      packets built and taken apart, frames put together across
 *            packets, the broken cases
 *   l2cap    a channel opened, configured both ways and closed, the other
 *            side's requests (refused connection, echo, information,
 *            configuration with an unknown option and a small MTU), LE's
 *            parameter update in and out of bounds, broken commands
 *   smp      a scripted responder (its values from crypto.c, which the
 *            vectors check): Secure Connections' Just Works, Numeric
 *            Comparison (yes and no) and Passkey Entry's twenty rounds,
 *            legacy Just Works with its keys, a short key, the debug key
 *            (the responder's and the controller's own), a reflected key,
 *            a wrong confirm, a failed DHKey
 *   keys     a bond written, read and forgotten in a new folder, broken
 *            files
 *   fuzz     random bytes into the ACL, L2CAP, SMP and bond parsers
 *
 * The P-256 keys are made-up ones of the test (not the Core's debug key):
 * their public keys and their DHKey were computed with python's
 * cryptography 43 (plan/ws143/phase004/phase.md section 9).
 *
 *   plan/ws143/tests/bt-daemon-host-test.sh
 */

#include "userland/base/bluetoothd/acl.h"
#include "userland/base/bluetoothd/crypto.h"
#include "userland/base/bluetoothd/keys.h"
#include "userland/base/bluetoothd/l2cap.h"
#include "userland/base/bluetoothd/smp.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The fuzz's rounds. */
#define TEST_FUZZ_ROUNDS	20000U

/* The checks that failed, and those that ran. */
static unsigned failures;
static unsigned checks;

/* The counter the test's random source counts from. */
static uint8_t random_counter;

/* The keys' folder (made by the script). */
static const char *keys_folder;

/*
 * The scripted responder of one SMP test: its IO capability and AuthReq,
 * the keys it distributes, its nonce, its view of the values, and the
 * passkey it types.
 */
struct peer {
	uint8_t io;
	uint8_t authentication;
	uint8_t key_size;
	uint8_t keys;
	int secure;
	uint8_t response[7];
	uint8_t nonce[16];
	uint32_t passkey;
	uint8_t mac_key[16];
	uint8_t ltk_msb[16];
};

static void expect(int condition, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void hex(const char *text, uint8_t *bytes);
static void reverse(uint8_t *to, const uint8_t *from, size_t length);
static void test_random(void *context, uint8_t *bytes, size_t length);
static void test_crypto(void);
static void test_acl(void);
static void test_l2cap(void);
static void test_l2cap_inbound(void);
static int accept_hook(void *context, uint16_t handle, uint16_t psm, uint16_t *result, uint16_t *status);
static void test_smp_secure(uint8_t peer_io, int agent_yes);
static void test_smp_legacy(void);
static void test_smp_failures(void);
static void test_keys(void);
static void test_fuzz(void);
static void peer_response(struct peer *peer, struct btd_smp *smp, unsigned *actions);
static void peer_confirm(const struct peer *peer, const uint8_t *nonce, uint8_t z, uint8_t *pdu);

/* The made-up keys: bluetoothd's controller's (A) and the responder's (B), X then Y most significant first, and their DHKey. */
static const char key_a_x[] = "e9f97fa093e059947e2b04296d94fcaedc7c39ccaee9c485fc201ae248b6c153";
static const char key_a_y[] = "076c1053224d5784a17567fe0ad585252185951f73bae27ae5c1786c4ba2c28b";
static const char key_b_x[] = "15246a54914ceab36f71fc8de373d8c453655170e3914031e75a3d7f728ebf84";
static const char key_b_y[] = "6f5d5e43a59cbd641f443cb01086c98180576f9be3b34818ab09a7c76adcb985";
static const char key_dh[] = "ac83af932027039b42c742d520774337c0bb9f1425ee556ff54ee960ebf9ea01";

/* The Core's debug public key (Vol 3 Part H §2.3.5.6.1), most significant first. */
static const char debug_x[] = "20b003d2f297be2c5e2c83a7e9f9a5b9eff49111acf4fddbcc0301480e359de6";
static const char debug_y[] = "dc809c49652aeb6d63329abf5a52155c766345c28fed3024741c8ed01589d28b";

/* The two addresses, least significant byte first: the initiator's (public) and the responder's (public). */
static const uint8_t address_a[6] = { 0x55U, 0x44U, 0x33U, 0x22U, 0x11U, 0x00U };
static const uint8_t address_b[6] = { 0x03U, 0x0eU, 0x0dU, 0x0cU, 0x0bU, 0x0aU };

/*
 * Runs every part; the exit status says whether every check held.
 */
int
main(
	int argc,
	char **argv)
{
	/* The keys' folder the script made. */
	keys_folder = "build/tmp";
	if (argc > 1)
		keys_folder = argv[1];

	/* The parts. */
	test_crypto();
	test_acl();
	test_l2cap();
	test_l2cap_inbound();
	test_smp_secure(0x03U, 1);
	test_smp_secure(0x01U, 1);
	test_smp_secure(0x01U, 0);
	test_smp_secure(0x02U, 1);
	test_smp_legacy();
	test_smp_failures();
	test_keys();
	test_fuzz();

	/* The verdict. */
	if (failures != 0U) {
		printf("bt-pair-host-test: FAIL (%u of %u checks)\n", failures, checks);
		return 1;
	}

	/* Succeeded: every check held. */
	printf("bt-pair-host-test: PASS (%u checks)\n", checks);
	return 0;
}

/* Counts a check, and prints it when it failed. */
static void
expect(
	int condition,
	const char *format,
	...)
{
	va_list arguments;

	/* One more check. */
	checks++;
	if (condition)
		return;

	/* A failure, named. */
	failures++;
	printf("FAIL: ");
	va_start(arguments, format);
	vprintf(format, arguments);
	va_end(arguments);
	printf("\n");
}

/* Reads hex digits into bytes. */
static void
hex(
	const char *text,
	uint8_t *bytes)
{
	unsigned value;
	size_t index;

	/* Two digits a byte. */
	for (index = 0U; text[2U * index] != '\0'; index++) {
		(void)sscanf(text + 2U * index, "%2x", &value);
		bytes[index] = (uint8_t)value;
	}
}

/* Copies bytes in the other order. */
static void
reverse(
	uint8_t *to,
	const uint8_t *from,
	size_t length)
{
	size_t index;

	/* The last first. */
	for (index = 0U; index < length; index++)
		to[index] = from[length - 1U - index];
}

/* The test's random source: a counter, so a run is the same every time. */
static void
test_random(
	void *context,
	uint8_t *bytes,
	size_t length)
{
	size_t index;

	/* Each byte the next count. */
	(void)context;
	for (index = 0U; index < length; index++) {
		random_counter = (uint8_t)(random_counter * 29U + 17U);
		bytes[index] = random_counter;
	}
}

/* FIPS-197, RFC 4493 and the Core's sample data. */
static void
test_crypto(void)
{
	uint8_t key[16];
	uint8_t input[64];
	uint8_t output[16];
	uint8_t expected[16];
	uint8_t u[32];
	uint8_t v[32];
	uint8_t x[16];
	uint8_t y[16];
	uint8_t w[32];
	uint8_t a1[7];
	uint8_t a2[7];
	uint8_t mac_key[16];
	uint8_t ltk[16];
	uint8_t r[16];
	uint8_t preq[7];
	uint8_t pres[7];
	uint8_t ia[6];
	uint8_t ra[6];
	uint8_t r1[16];
	uint8_t r2[16];
	uint8_t hash[3];
	static const uint8_t io[3] = { 0x01U, 0x01U, 0x02U };

	/* FIPS-197 Appendix C.1. */
	hex("000102030405060708090a0b0c0d0e0f", key);
	hex("00112233445566778899aabbccddeeff", input);
	hex("69c4e0d86a7b0430d8cdb78070b4c55a", expected);
	btd_aes_encrypt(key, input, output);
	expect(memcmp(output, expected, 16U) == 0, "crypto: FIPS-197 C.1");

	/* RFC 4493 §4: the empty message, 16, 40 and 64 bytes. */
	hex("2b7e151628aed2a6abf7158809cf4f3c", key);
	hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710", input);
	hex("bb1d6929e95937287fa37d129b756746", expected);
	btd_cmac(key, input, 0U, output);
	expect(memcmp(output, expected, 16U) == 0, "crypto: RFC 4493 example 1");
	hex("070a16b46b4d4144f79bdd9dd04a287c", expected);
	btd_cmac(key, input, 16U, output);
	expect(memcmp(output, expected, 16U) == 0, "crypto: RFC 4493 example 2");
	hex("dfa66747de9ae63030ca32611497c827", expected);
	btd_cmac(key, input, 40U, output);
	expect(memcmp(output, expected, 16U) == 0, "crypto: RFC 4493 example 3");
	hex("51f0bebf7e3b9d92fc49741779363cfe", expected);
	btd_cmac(key, input, 64U, output);
	expect(memcmp(output, expected, 16U) == 0, "crypto: RFC 4493 example 4");

	/* f4, g2, f5, f6 (Core Vol 3 Part H D.2 to D.5). */
	hex("20b003d2f297be2c5e2c83a7e9f9a5b9eff49111acf4fddbcc0301480e359de6", u);
	hex("55188b3d32f6bb9a900afcfbeed4e72a59cb9ac2f19d7cfb6b4fdd49f47fc5fd", v);
	hex("d5cb8454d177733effffb2ec712baeab", x);
	hex("a6e8e7cc25a75f6e216583f7ff3dc4cf", y);
	hex("f2c916f107a9bd1cf1eda1bea974872d", expected);
	btd_smp_f4(u, v, x, 0U, output);
	expect(memcmp(output, expected, 16U) == 0, "crypto: f4");
	expect(btd_smp_g2(u, v, x, y) == 0x2f9ed5baU, "crypto: g2");
	hex("ec0234a357c8ad05341010a60a397d9b99796b13b4f866f1868d34f373bfa698", w);
	hex("0056123737bfce", a1);
	hex("00a713702dcfc1", a2);
	btd_smp_f5(w, x, y, a1, a2, mac_key, ltk);
	hex("2965f176a1084a02fd3f6a20ce636e20", expected);
	expect(memcmp(mac_key, expected, 16U) == 0, "crypto: f5's MacKey");
	hex("6986791169d7cd23980522b594750a38", expected);
	expect(memcmp(ltk, expected, 16U) == 0, "crypto: f5's LTK");
	hex("12a3343bb453bb5408da42d20c2d0fc8", r);
	hex("e3c473989cd0e8c5d26c0b09da958f61", expected);
	btd_smp_f6(mac_key, x, y, r, io, a1, a2, output);
	expect(memcmp(output, expected, 16U) == 0, "crypto: f6");

	/* c1, s1 and ah (Core Vol 3 Part H D.1, §2.2.3 to §2.2.5's examples). */
	memset(key, 0, sizeof(key));
	hex("5783d52156ad6f0e6388274ec6702ee0", r);
	hex("07071000000101", preq);
	hex("05000800000302", pres);
	hex("a1a2a3a4a5a6", ia);
	hex("b1b2b3b4b5b6", ra);
	hex("1e1e3fef878988ead2a74dc5bef13b86", expected);
	btd_smp_c1(key, r, pres, preq, 0U, 1U, ia, ra, output);
	expect(memcmp(output, expected, 16U) == 0, "crypto: c1");
	hex("000f0e0d0c0b0a091122334455667788", r1);
	hex("010203040506070899aabbccddeeff00", r2);
	hex("9a1fe1f0e8b0f49b5b4216ae796da062", expected);
	btd_smp_s1(key, r1, r2, output);
	expect(memcmp(output, expected, 16U) == 0, "crypto: s1");
	hex("ec0234a357c8ad05341010a60a397d9b", key);
	hex("708194", input);
	btd_smp_ah(key, input, hash);
	expect(hash[0] == 0x0dU && hash[1] == 0xfbU && hash[2] == 0xaaU, "crypto: ah");
}

/* ACL packets and frames. */
static void
test_acl(void)
{
	struct btd_reassembly reassembly;
	struct btd_acl acl;
	uint8_t packet[64];
	uint8_t frame[32];
	size_t length;
	int error;
	int whole;

	/* A packet: handle 0x041, a first flushable packet, four bytes. */
	length = btd_acl_build(packet, sizeof(packet), 0x0041U, BTD_ACL_FIRST_FLUSHABLE, (const uint8_t *)"\x05\x00\x01\x00", 4U);
	error = btd_acl_parse(packet, length, &acl);
	expect(length == 9U && packet[2] == 0x20U && error == 0 && acl.handle == 0x0041U && acl.boundary == BTD_ACL_FIRST_FLUSHABLE && acl.length == 4U,
	       "acl: a packet built and read");
	packet[3] = 9U;
	error = btd_acl_parse(packet, length, &acl);
	expect(error == EBADMSG, "acl: a length that lies");

	/* A frame of five bytes on CID 6 in two packets. */
	memset(&reassembly, 0, sizeof(reassembly));
	length = btd_l2cap_frame(frame, sizeof(frame), 0x0006U, (const uint8_t *)"\x01\x02\x03\x04\x05", 5U);
	acl.boundary = BTD_ACL_FIRST;
	acl.data = frame;
	acl.length = 6U;
	whole = btd_reassembly_feed(&reassembly, &acl);
	expect(whole == 0, "acl: the first part waits");
	acl.boundary = BTD_ACL_CONTINUING;
	acl.data = frame + 6;
	acl.length = length - 6U;
	whole = btd_reassembly_feed(&reassembly, &acl);
	expect(whole == 1 && reassembly.expected == 9U && reassembly.frame[8] == 0x05U, "acl: the frame is whole");

	/* A continuing packet with no frame, more than the frame's length, a frame too long, a header cut. */
	whole = btd_reassembly_feed(&reassembly, &acl);
	expect(whole == -1, "acl: a continuing packet with no frame is dropped");
	acl.boundary = BTD_ACL_FIRST;
	acl.data = frame;
	acl.length = length;
	frame[0] = 2U;
	whole = btd_reassembly_feed(&reassembly, &acl);
	expect(whole == -1, "acl: data past the frame's length is dropped");
	frame[0] = 0xffU;
	frame[1] = 0x7fU;
	whole = btd_reassembly_feed(&reassembly, &acl);
	expect(whole == -1, "acl: a frame longer than the most is dropped");
	acl.length = 3U;
	whole = btd_reassembly_feed(&reassembly, &acl);
	expect(whole == -1 && reassembly.dropped >= 4U, "acl: a first packet without the L2CAP header is dropped");
}

/*
 * The owner's answer of the inbound test (ws143-p005): PSM 0x0011
 * accepted, 0x0013 pending, 0x0015 left to the default (the hook says no
 * answer), anything else PSM not supported.
 */
static int
accept_hook(
	void *context,
	uint16_t handle,
	uint16_t psm,
	uint16_t *result,
	uint16_t *status)
{
	unsigned *calls;

	/* Counted. */
	(void)handle;
	calls = context;
	(*calls)++;
	*status = 0U;

	/* Each PSM's answer. */
	if (psm == 0x0011U) {
		*result = BTD_L2CAP_SUCCESS;
	} else if (psm == 0x0013U) {
		*result = BTD_L2CAP_PENDING;
	} else if (psm == 0x0015U) {
		return 1;
	} else {
		*result = BTD_L2CAP_PSM_NOT_SUPPORTED;
	}

	/* Answered. */
	return 0;
}

/*
 * L2CAP's channels the other side asks for (ws143-p005, phase005 sections
 * 4.2 and 9.8): accepted, pending then answered, refused; the options of
 * HID's devices; opened and closed; the Echo Request.
 */
static void
test_l2cap_inbound(void)
{
	struct btd_signal_effect effect;
	struct btd_channel *channel;
	struct btd_l2cap l2cap;
	uint8_t answer[BTD_SIGNAL_MAX];
	uint8_t command[80];
	size_t length;
	unsigned calls;
	unsigned index;
	uint16_t cid;
	int error;

	/* The owner's hook. */
	btd_l2cap_init(&l2cap);
	calls = 0U;
	btd_l2cap_set_accept(&l2cap, accept_hook, &calls);

	/* PSM 0x11 accepted: the response with our CID 0x40, then our Configure Request to theirs (0x41). */
	memcpy(command, "\x02\x07\x04\x00\x11\x00\x41\x00", 8U);
	error = btd_l2cap_signal(&l2cap, 0x0040U, 0, command, 8U, answer, sizeof(answer), &length, &effect);
	channel = btd_l2cap_channel(&l2cap, 0x0040U);
	expect(error == 0 && calls == 1U && length == 24U && answer[0] == 0x03U && answer[1] == 0x07U && answer[4] == 0x40U &&
	       answer[6] == 0x41U && answer[8] == 0x00U && answer[12] == 0x04U && answer[16] == 0x41U &&
	       channel != NULL && channel->inbound && channel->state == BTD_CHANNEL_CONFIGURING,
	       "l2cap-in: accepted, configuring (%zu)", length);

	/* Their Configure Request: the MTU, a Flush Timeout, a QoS, an FCS, all taken. */
	memset(command, 0, sizeof(command));
	memcpy(command, "\x04\x08\x27\x00\x40\x00\x00\x00" "\x01\x02\xa0\x02" "\x02\x02\xff\xff" "\x03\x16", 18U);
	command[18 + 22] = 0x05U;
	command[18 + 23] = 0x01U;
	command[18 + 24] = 0x00U;
	error = btd_l2cap_signal(&l2cap, 0x0040U, 0, command, 43U, answer, sizeof(answer), &length, &effect);
	expect(error == 0 && answer[0] == 0x05U && answer[8] == 0x00U && channel->have_flush_timeout && channel->flush_timeout == 0xffffU &&
	       channel->have_qos && channel->remote_done && effect.opened_count == 0U,
	       "l2cap-in: Flush Timeout, QoS and FCS taken");

	/* Their answer to ours: open, and told. */
	memcpy(command, "\x05\x01\x06\x00\x40\x00\x00\x00\x00\x00", 10U);
	command[1] = 0x01U;
	error = btd_l2cap_signal(&l2cap, 0x0040U, 0, command, 10U, answer, sizeof(answer), &length, &effect);
	expect(error == 0 && channel->state == BTD_CHANNEL_OPEN && effect.opened_count == 1U && effect.opened[0] == 0x0040U,
	       "l2cap-in: open, told");

	/* PSM 0x13 pending: the response says so, nothing more. */
	memcpy(command, "\x02\x09\x04\x00\x13\x00\x42\x00", 8U);
	error = btd_l2cap_signal(&l2cap, 0x0040U, 0, command, 8U, answer, sizeof(answer), &length, &effect);
	channel = btd_l2cap_channel(&l2cap, 0x0041U);
	expect(error == 0 && length == 12U && answer[4] == 0x41U && answer[8] == 0x01U && answer[10] == 0x00U &&
	       channel != NULL && channel->state == BTD_CHANNEL_PENDING,
	       "l2cap-in: pending");

	/* A configuration of a pending channel is not taken. */
	memcpy(command, "\x04\x0a\x04\x00\x41\x00\x00\x00", 8U);
	error = btd_l2cap_signal(&l2cap, 0x0040U, 0, command, 8U, answer, sizeof(answer), &length, &effect);
	expect(error == 0 && answer[0] == 0x01U, "l2cap-in: a pending channel is not configured");

	/* The encryption done: the final answer under their identifier, then our Configure Request. */
	error = btd_l2cap_answer_pending(&l2cap, 0x0040U, BTD_L2CAP_SUCCESS, answer, sizeof(answer), &length);
	expect(error == 0 && length == 24U && answer[0] == 0x03U && answer[1] == 0x09U && answer[4] == 0x41U && answer[8] == 0x00U &&
	       answer[12] == 0x04U && channel->state == BTD_CHANNEL_CONFIGURING,
	       "l2cap-in: pending, then accepted");
	error = btd_l2cap_answer_pending(&l2cap, 0x0040U, BTD_L2CAP_SUCCESS, answer, sizeof(answer), &length);
	expect(error == 0 && length == 0U, "l2cap-in: nothing pending any more");

	/* Another pending one refused for security: no CID of ours, the slot free. */
	memcpy(command, "\x02\x0b\x04\x00\x13\x00\x43\x00", 8U);
	(void)btd_l2cap_signal(&l2cap, 0x0040U, 0, command, 8U, answer, sizeof(answer), &length, &effect);
	cid = (uint16_t)(answer[4] | (answer[5] << 8));
	error = btd_l2cap_answer_pending(&l2cap, 0x0040U, BTD_L2CAP_SECURITY_BLOCK, answer, sizeof(answer), &length);
	expect(error == 0 && length == 12U && answer[1] == 0x0bU && answer[4] == 0x00U && answer[8] == 0x03U &&
	       btd_l2cap_channel(&l2cap, cid) == NULL,
	       "l2cap-in: pending, then a security block");

	/* A source CID that is not dynamic, one taken already, and a PSM the hook leaves alone. */
	memcpy(command, "\x02\x0c\x04\x00\x11\x00\x01\x00", 8U);
	(void)btd_l2cap_signal(&l2cap, 0x0040U, 0, command, 8U, answer, sizeof(answer), &length, &effect);
	expect(answer[8] == 0x06U && answer[4] == 0x00U, "l2cap-in: an invalid source CID");
	memcpy(command, "\x02\x0d\x04\x00\x11\x00\x41\x00", 8U);
	(void)btd_l2cap_signal(&l2cap, 0x0040U, 0, command, 8U, answer, sizeof(answer), &length, &effect);
	expect(answer[8] == 0x07U, "l2cap-in: a source CID already taken");
	memcpy(command, "\x02\x0e\x04\x00\x15\x00\x50\x00", 8U);
	(void)btd_l2cap_signal(&l2cap, 0x0040U, 0, command, 8U, answer, sizeof(answer), &length, &effect);
	expect(answer[8] == 0x02U, "l2cap-in: a PSM the hook leaves is not supported");

	/* Retransmission other than the basic mode: unacceptable, the basic mode offered. */
	memcpy(command, "\x02\x0f\x04\x00\x11\x00\x51\x00", 8U);
	(void)btd_l2cap_signal(&l2cap, 0x0040U, 0, command, 8U, answer, sizeof(answer), &length, &effect);
	cid = (uint16_t)(answer[4] | (answer[5] << 8));
	memset(command, 0, sizeof(command));
	memcpy(command, "\x04\x10\x0f\x00\x00\x00\x00\x00\x04\x09\x03", 11U);
	command[4] = (uint8_t)(cid & 0xffU);
	error = btd_l2cap_signal(&l2cap, 0x0040U, 0, command, 19U, answer, sizeof(answer), &length, &effect);
	expect(error == 0 && answer[0] == 0x05U && answer[8] == 0x01U && answer[10] == 0x04U && answer[11] == 0x09U && answer[12] == 0x00U &&
	       length == 4U + 6U + 11U,
	       "l2cap-in: ERTM unacceptable, basic offered (%zu)", length);

	/* Their close of the open channel, from another connection (passed over) and from its own (told). */
	memcpy(command, "\x06\x11\x04\x00\x40\x00\x41\x00", 8U);
	(void)btd_l2cap_signal(&l2cap, 0x0041U, 0, command, 8U, answer, sizeof(answer), &length, &effect);
	expect(btd_l2cap_channel(&l2cap, 0x0040U) != NULL && effect.closed_count == 0U, "l2cap-in: another connection cannot close it");
	(void)btd_l2cap_signal(&l2cap, 0x0040U, 0, command, 8U, answer, sizeof(answer), &length, &effect);
	expect(btd_l2cap_channel(&l2cap, 0x0040U) == NULL && effect.closed_count == 1U && effect.closed[0] == 0x0040U &&
	       effect.closed_reason[0] == BTD_L2CAP_CLOSED_REMOTE && answer[0] == 0x07U,
	       "l2cap-in: closed by them, told");

	/* The Echo Request, and only its own answer. */
	error = btd_l2cap_echo(&l2cap, answer, sizeof(answer), &length);
	memcpy(command, "\x09\x00\x00\x00", 4U);
	command[1] = (uint8_t)(answer[1] + 1U);
	expect(error == 0 && length == 4U && answer[0] == 0x08U, "l2cap-in: the Echo Request");
	(void)btd_l2cap_signal(&l2cap, 0x0040U, 0, command, 4U, answer + 8, sizeof(answer) - 8U, &length, &effect);
	expect(!effect.echo, "l2cap-in: another identifier's echo is not ours");
	command[1] = answer[1];
	(void)btd_l2cap_signal(&l2cap, 0x0040U, 0, command, 4U, answer + 8, sizeof(answer) - 8U, &length, &effect);
	expect(effect.echo && !l2cap.echo_pending, "l2cap-in: the echo came");

	/* A full table: no resources. */
	btd_l2cap_init(&l2cap);
	btd_l2cap_set_accept(&l2cap, accept_hook, &calls);
	for (index = 0U; index < BTD_CHANNELS_MAX; index++)
		(void)btd_l2cap_connect(&l2cap, 0x0040U, 0x0001U, answer, sizeof(answer), &length, &cid);
	memcpy(command, "\x02\x12\x04\x00\x11\x00\x60\x00", 8U);
	(void)btd_l2cap_signal(&l2cap, 0x0040U, 0, command, 8U, answer, sizeof(answer), &length, &effect);
	expect(answer[8] == 0x04U && answer[4] == 0x00U, "l2cap-in: no resources");
}

/* L2CAP's signalling. */
static void
test_l2cap(void)
{
	struct btd_signal_effect effect;
	struct btd_channel *channel;
	struct btd_l2cap l2cap;
	uint8_t answer[BTD_SIGNAL_MAX];
	uint8_t command[64];
	size_t length;
	uint16_t cid;
	int error;

	/* Our channel to PSM 1: the Connection Request. */
	btd_l2cap_init(&l2cap);
	error = btd_l2cap_connect(&l2cap, 0x0040U, 0x0001U, answer, sizeof(answer), &length, &cid);
	expect(error == 0 && length == 8U && answer[0] == 0x02U && answer[4] == 0x01U && answer[6] == 0x40U && cid == 0x0040U,
	       "l2cap: the Connection Request");

	/* Their success: our Configure Request with the MTU. */
	memcpy(command, "\x03\x01\x08\x00\x77\x00\x40\x00\x00\x00\x00\x00", 12U);
	error = btd_l2cap_signal(&l2cap, 0x0040U, 0, command, 12U, answer, sizeof(answer), &length, &effect);
	channel = btd_l2cap_channel(&l2cap, cid);
	expect(error == 0 && length == 12U && answer[0] == 0x04U && answer[4] == 0x77U && answer[8] == 0x01U && answer[10] == 0xa0U &&
	       channel != NULL && channel->state == BTD_CHANNEL_CONFIGURING,
	       "l2cap: connected, configuring");

	/* Their request (MTU 100) and their answer to ours, in one frame: open. */
	memcpy(command, "\x04\x05\x08\x00\x40\x00\x00\x00\x01\x02\x64\x00" "\x05\x02\x06\x00\x40\x00\x00\x00\x00\x00", 22U);
	error = btd_l2cap_signal(&l2cap, 0x0040U, 0, command, 22U, answer, sizeof(answer), &length, &effect);
	expect(error == 0 && length == 10U && answer[0] == 0x05U && answer[4] == 0x77U && answer[8] == 0x00U &&
	       channel->state == BTD_CHANNEL_OPEN && channel->remote_mtu == 100U,
	       "l2cap: configured both ways, open");

	/* Ours closed. */
	error = btd_l2cap_disconnect(&l2cap, cid, answer, sizeof(answer), &length);
	expect(error == 0 && answer[0] == 0x06U && answer[4] == 0x77U && answer[6] == 0x40U, "l2cap: the Disconnection Request");
	memcpy(command, "\x07\x09\x04\x00\x77\x00\x40\x00", 8U);
	error = btd_l2cap_signal(&l2cap, 0x0040U, 0, command, 8U, answer, sizeof(answer), &length, &effect);
	expect(error == 0 && length == 0U && btd_l2cap_channel(&l2cap, cid) == NULL, "l2cap: closed");

	/* Their Connection Request: refused (PSM not supported). */
	memcpy(command, "\x02\x07\x04\x00\x11\x00\x41\x00", 8U);
	error = btd_l2cap_signal(&l2cap, 0x0040U, 0, command, 8U, answer, sizeof(answer), &length, &effect);
	expect(error == 0 && answer[0] == 0x03U && answer[1] == 0x07U && answer[6] == 0x41U && answer[8] == 0x02U, "l2cap: their channel refused");

	/* Echo, information (features, fixed channels, another), and an unknown command. */
	memcpy(command, "\x08\x0a\x02\x00\xab\xcd" "\x0a\x0b\x02\x00\x02\x00" "\x0a\x0c\x02\x00\x03\x00" "\x0a\x0d\x02\x00\x01\x00" "\x7e\x0e\x00\x00", 28U);
	error = btd_l2cap_signal(&l2cap, 0x0040U, 0, command, 28U, answer, sizeof(answer), &length, &effect);
	expect(error == 0 && answer[0] == 0x09U && answer[4] == 0xabU &&
	       answer[6] == 0x0bU && answer[14] == 0x80U &&
	       answer[18] == 0x0bU && answer[26] == 0x02U &&
	       answer[34] == 0x0bU && answer[40] == 0x01U &&
	       answer[42] == 0x01U && answer[43] == 0x0eU,
	       "l2cap: echo, information, reject");

	/* A configuration with an unknown option and one with a small MTU (a channel configuring). */
	error = btd_l2cap_connect(&l2cap, 0x0040U, 0x0001U, answer, sizeof(answer), &length, &cid);
	memcpy(command, "\x03\x01\x08\x00\x78\x00\x40\x00\x00\x00\x00\x00", 12U);
	command[1] = answer[1];
	(void)btd_l2cap_signal(&l2cap, 0x0040U, 0, command, 12U, answer, sizeof(answer), &length, &effect);
	memcpy(command, "\x04\x10\x08\x00\x40\x00\x00\x00\x05\x02\x00\x00", 12U);
	error = btd_l2cap_signal(&l2cap, 0x0040U, 0, command, 12U, answer, sizeof(answer), &length, &effect);
	expect(error == 0 && answer[0] == 0x05U && answer[8] == 0x03U && answer[10] == 0x05U, "l2cap: an unknown option refused with its type");
	memcpy(command, "\x04\x11\x08\x00\x40\x00\x00\x00\x01\x02\x20\x00", 12U);
	error = btd_l2cap_signal(&l2cap, 0x0040U, 0, command, 12U, answer, sizeof(answer), &length, &effect);
	expect(error == 0 && answer[8] == 0x01U && answer[10] == 0x01U && answer[12] == 48U, "l2cap: a small MTU refused with the least");

	/* LE: a parameter update in bounds is accepted and asked of the caller; one out of bounds is rejected. */
	memcpy(command, "\x12\x01\x08\x00\x18\x00\x28\x00\x00\x00\x2a\x00", 12U);
	error = btd_l2cap_signal(&l2cap, 0x0041U, 1, command, 12U, answer, sizeof(answer), &length, &effect);
	expect(error == 0 && answer[0] == 0x13U && answer[4] == 0U && effect.update && effect.interval_max == 0x28U, "l2cap: LE's update accepted");
	memcpy(command, "\x12\x02\x08\x00\x18\x00\x28\x00\x00\x00\x05\x00", 12U);
	error = btd_l2cap_signal(&l2cap, 0x0041U, 1, command, 12U, answer, sizeof(answer), &length, &effect);
	expect(error == 0 && answer[4] == 1U && !effect.update, "l2cap: LE's update out of bounds rejected");
	memcpy(command, "\x02\x03\x04\x00\x80\x00\x40\x00", 8U);
	error = btd_l2cap_signal(&l2cap, 0x0041U, 1, command, 8U, answer, sizeof(answer), &length, &effect);
	expect(error == 0 && answer[0] == 0x01U, "l2cap: LE rejects BR/EDR's commands");

	/* A command whose length runs past the frame. */
	memcpy(command, "\x0a\x0f\x09\x00\x02\x00", 6U);
	error = btd_l2cap_signal(&l2cap, 0x0040U, 0, command, 6U, answer, sizeof(answer), &length, &effect);
	expect(error == -1 && l2cap.malformed == 1U, "l2cap: a command past the frame");
}

/* The responder's Pairing Response, given to bluetoothd. */
static void
peer_response(
	struct peer *peer,
	struct btd_smp *smp,
	unsigned *actions)
{
	/* The PDU: its IO capability, no OOB, its AuthReq, its key size, no keys from the initiator, its keys. */
	peer->response[0] = 0x02U;
	peer->response[1] = peer->io;
	peer->response[2] = 0x00U;
	peer->response[3] = peer->authentication;
	peer->response[4] = peer->key_size;
	peer->response[5] = 0x00U;
	peer->response[6] = peer->keys;
	*actions = btd_smp_input(smp, peer->response, 7U);
}

/* Writes the responder's confirm PDU: f4(PKbx, PKax, nonce, z). */
static void
peer_confirm(
	const struct peer *peer,
	const uint8_t *nonce,
	uint8_t z,
	uint8_t *pdu)
{
	uint8_t a_x[32];
	uint8_t b_x[32];
	uint8_t nonce_msb[16];
	uint8_t confirm[16];

	/* The values. */
	(void)peer;
	hex(key_a_x, a_x);
	hex(key_b_x, b_x);
	reverse(nonce_msb, nonce, 16U);

	/* The PDU. */
	btd_smp_f4(b_x, a_x, nonce_msb, z, confirm);
	pdu[0] = 0x03U;
	reverse(pdu + 1, confirm, 16U);
}

/*
 * A Secure Connections pairing against a responder of an IO capability:
 * 0x03 Just Works, 0x01 Numeric Comparison (the agent says yes or no),
 * 0x02 Passkey Entry (bluetoothd shows, the responder types).
 */
static void
test_smp_secure(
	uint8_t peer_io,
	int agent_yes)
{
	struct btd_smp smp;
	struct peer peer;
	uint8_t key[64];
	uint8_t dhkey[32];
	uint8_t pdu[65];
	uint8_t w[32];
	uint8_t na[16];
	uint8_t nb[16];
	uint8_t a[7];
	uint8_t b[7];
	uint8_t r[16];
	uint8_t io[3];
	uint8_t check[16];
	uint8_t a_x[32];
	uint8_t b_x[32];
	uint8_t ltk[16];
	uint8_t received_nonce[16];
	uint8_t nonce_msb[16];
	uint8_t expected[16];
	uint8_t got[16];
	unsigned actions;
	unsigned round;
	unsigned rounds;
	uint8_t z;
	uint32_t number;

	/* The pairing, and the responder. */
	random_counter = 0U;
	btd_smp_init(&smp, BTD_SMP_DISPLAY_YES_NO, 0U, address_a, 0U, address_b, test_random, NULL);
	memset(&peer, 0, sizeof(peer));
	peer.io = peer_io;
	peer.authentication = 0x0dU;
	peer.key_size = 16U;
	peer.keys = 0x02U;
	actions = btd_smp_start(&smp);
	expect(actions == BTD_SMP_SEND && smp.out_length == 7U && smp.out[0] == 0x01U && smp.out[3] == 0x0dU && smp.out[6] == 0x03U,
	       "smp %u: the Pairing Request", peer_io);

	/* The response: the controller's key is asked for (and the passkey shown for Passkey Entry). */
	peer_response(&peer, &smp, &actions);
	expect((actions & BTD_SMP_READ_KEY) != 0U && smp.secure, "smp %u: Secure Connections, the key asked of the controller", peer_io);
	if (peer_io == 0x02U)
		expect((actions & BTD_SMP_SHOW) != 0U && smp.number < 1000000U, "smp 2: the passkey shown");
	peer.passkey = smp.passkey;

	/* The controller's key, sent. */
	hex(key_a_x, key);
	hex(key_a_y, key + 32);
	reverse(pdu, key, 32U);
	reverse(pdu + 32, key + 32, 32U);
	actions = btd_smp_local_key(&smp, 0U, pdu);
	expect(actions == BTD_SMP_SEND && smp.out[0] == 0x0cU && memcmp(smp.out + 1, pdu, 64U) == 0, "smp %u: our public key", peer_io);

	/* The responder's key: the DHKey asked for (Passkey Entry's first confirm with it). */
	hex(key_b_x, key);
	hex(key_b_y, key + 32);
	pdu[0] = 0x0cU;
	reverse(pdu + 1, key, 32U);
	reverse(pdu + 33, key + 32, 32U);
	actions = btd_smp_input(&smp, pdu, 65U);
	expect((actions & BTD_SMP_DHKEY) != 0U, "smp %u: the DHKey asked for", peer_io);
	hex(key_dh, w);
	reverse(dhkey, w, 32U);
	(void)btd_smp_dhkey(&smp, 0U, dhkey);

	/* The rounds: one for Just Works and Numeric Comparison, twenty for Passkey Entry. */
	hex(key_a_x, a_x);
	hex(key_b_x, b_x);
	rounds = 1U;
	if (peer_io == 0x02U)
		rounds = 20U;
	for (round = 0U; round < rounds; round++) {
		/* The responder's nonce and the round's bit. */
		test_random(NULL, peer.nonce, 16U);
		z = 0U;
		if (peer_io == 0x02U)
			z = (uint8_t)(0x80U | ((peer.passkey >> round) & 1U));

		/* Passkey Entry: bluetoothd's confirm came first; it is checked when its nonce comes. */
		if (peer_io == 0x02U) {
			memcpy(check, smp.out + 1, 16U);
		}

		/* The responder's confirm; bluetoothd answers with its nonce. */
		peer_confirm(&peer, peer.nonce, z, pdu);
		actions = btd_smp_input(&smp, pdu, 17U);
		expect(actions == BTD_SMP_SEND && smp.out[0] == 0x04U, "smp %u round %u: our nonce", peer_io, round);
		memcpy(received_nonce, smp.out + 1, 16U);

		/* Passkey Entry: bluetoothd's confirm must be f4(PKax, PKbx, Nai, rai). */
		if (peer_io == 0x02U) {
			reverse(nonce_msb, received_nonce, 16U);
			btd_smp_f4(a_x, b_x, nonce_msb, z, expected);
			reverse(got, check, 16U);
			expect(memcmp(expected, got, 16U) == 0, "smp 2 round %u: our confirm is f4 of our nonce and the bit", round);
		}

		/* The responder's nonce. */
		pdu[0] = 0x04U;
		memcpy(pdu + 1, peer.nonce, 16U);
		actions = btd_smp_input(&smp, pdu, 17U);
	}

	/* Numeric Comparison: the number is g2's, and the agent answers. */
	reverse(na, received_nonce, 16U);
	reverse(nb, peer.nonce, 16U);
	if (peer_io == 0x01U) {
		number = btd_smp_g2(a_x, b_x, na, nb) % 1000000U;
		expect((actions & BTD_SMP_CONFIRM) != 0U && smp.number == number, "smp 1: the number to confirm (%06u)", (unsigned)number);
		actions = btd_smp_agent(&smp, agent_yes);
		if (!agent_yes) {
			expect((actions & BTD_SMP_FAILED) != 0U && smp.failure == 0x0cU && smp.out[0] == 0x05U, "smp 1: no is Numeric Comparison Failed");
			return;
		}
	}

	/* bluetoothd's check: Ea = f6(MacKey, Na, Nb, rb, IOcapA, A, B). */
	expect((actions & BTD_SMP_SEND) != 0U && smp.out[0] == 0x0dU, "smp %u: our DHKey check", peer_io);
	a[0] = 0U;
	reverse(a + 1, address_a, 6U);
	b[0] = 0U;
	reverse(b + 1, address_b, 6U);
	hex(key_dh, w);
	btd_smp_f5(w, na, nb, a, b, peer.mac_key, peer.ltk_msb);
	memset(r, 0, sizeof(r));
	if (peer_io == 0x02U) {
		r[12] = (uint8_t)(peer.passkey >> 24);
		r[13] = (uint8_t)(peer.passkey >> 16);
		r[14] = (uint8_t)(peer.passkey >> 8);
		r[15] = (uint8_t)peer.passkey;
	}

	/* bluetoothd's IO capability: AuthReq, no OOB, DisplayYesNo. */
	io[0] = 0x0dU;
	io[1] = 0x00U;
	io[2] = 0x01U;
	btd_smp_f6(peer.mac_key, na, nb, r, io, a, b, check);
	reverse(pdu, check, 16U);
	expect(memcmp(pdu, smp.out + 1, 16U) == 0, "smp %u: Ea is right", peer_io);

	/* The responder's check: Eb = f6(MacKey, Nb, Na, ra, IOcapB, B, A); the encryption with the LTK. */
	io[0] = peer.authentication;
	io[1] = 0x00U;
	io[2] = peer.io;
	btd_smp_f6(peer.mac_key, nb, na, r, io, b, a, check);
	pdu[0] = 0x0dU;
	reverse(pdu + 1, check, 16U);
	actions = btd_smp_input(&smp, pdu, 17U);
	reverse(ltk, peer.ltk_msb, 16U);
	expect(actions == BTD_SMP_ENCRYPT && memcmp(smp.encrypt_key, ltk, 16U) == 0 && smp.encrypt_ediv == 0U, "smp %u: encrypted with the LTK", peer_io);

	/* Encrypted: the identity keys, then the end. */
	actions = btd_smp_encrypted(&smp, 0U, 1);
	expect(actions == 0U, "smp %u: the identity awaited", peer_io);
	memset(pdu, 0x5aU, 17U);
	pdu[0] = 0x08U;
	actions = btd_smp_input(&smp, pdu, 17U);
	expect(actions == 0U, "smp %u: the IRK taken", peer_io);
	memcpy(pdu, "\x09\x00\x03\x0e\x0d\x0c\x0b\x0a", 8U);
	actions = btd_smp_input(&smp, pdu, 8U);
	expect(actions == BTD_SMP_DONE && smp.keys.have_irk && smp.keys.irk[0] == 0x5aU && smp.keys.have_identity && smp.keys.secure &&
	       smp.keys.authenticated == (peer_io != 0x03U),
	       "smp %u: paired, the keys kept", peer_io);
}

/* Legacy Just Works with the responder's LTK and identity. */
static void
test_smp_legacy(void)
{
	struct btd_smp smp;
	struct peer peer;
	uint8_t pdu[17];
	uint8_t tk[16];
	uint8_t preq[7];
	uint8_t pres[7];
	uint8_t ia[6];
	uint8_t ra[6];
	uint8_t mrand[16];
	uint8_t srand[16];
	uint8_t confirm[16];
	uint8_t expected[16];
	uint8_t stk[16];
	unsigned actions;

	/* A responder without Secure Connections nor MITM, giving EncKey and IdKey. */
	random_counter = 0U;
	btd_smp_init(&smp, BTD_SMP_NO_IO, 0U, address_a, 0U, address_b, test_random, NULL);
	memset(&peer, 0, sizeof(peer));
	peer.io = 0x03U;
	peer.authentication = 0x01U;
	peer.key_size = 16U;
	peer.keys = 0x03U;
	(void)btd_smp_start(&smp);
	peer_response(&peer, &smp, &actions);
	expect(actions == BTD_SMP_SEND && smp.out[0] == 0x03U && !smp.secure, "legacy: our confirm first");

	/* Mconfirm must be c1(0, Mrand, ...), which the responder checks once Mrand comes. */
	memcpy(confirm, smp.out + 1, 16U);
	test_random(NULL, srand, 16U);
	memset(tk, 0, sizeof(tk));
	reverse(preq, smp.request, 7U);
	reverse(pres, peer.response, 7U);
	reverse(ia, address_a, 6U);
	reverse(ra, address_b, 6U);
	btd_smp_c1(tk, srand, pres, preq, 0U, 0U, ia, ra, expected);
	pdu[0] = 0x03U;
	reverse(pdu + 1, expected, 16U);
	actions = btd_smp_input(&smp, pdu, 17U);
	expect(actions == BTD_SMP_SEND && smp.out[0] == 0x04U, "legacy: our nonce");
	reverse(mrand, smp.out + 1, 16U);
	btd_smp_c1(tk, mrand, pres, preq, 0U, 0U, ia, ra, expected);
	reverse(pdu, confirm, 16U);
	expect(memcmp(pdu, expected, 16U) == 0, "legacy: Mconfirm is c1 of Mrand");

	/* Srand: the STK = s1(TK, Srand, Mrand). */
	pdu[0] = 0x04U;
	reverse(pdu + 1, srand, 16U);
	actions = btd_smp_input(&smp, pdu, 17U);
	btd_smp_s1(tk, srand, mrand, stk);
	reverse(expected, stk, 16U);
	expect(actions == BTD_SMP_ENCRYPT && memcmp(smp.encrypt_key, expected, 16U) == 0, "legacy: encrypted with the STK");

	/* The keys: LTK, EDIV and Rand, IRK, identity; then the end. */
	(void)btd_smp_encrypted(&smp, 0U, 1);
	memset(pdu, 0x11U, 17U);
	pdu[0] = 0x06U;
	actions = btd_smp_input(&smp, pdu, 17U);
	memcpy(pdu, "\x07\x34\x12\x01\x02\x03\x04\x05\x06\x07\x08", 11U);
	actions = btd_smp_input(&smp, pdu, 11U);
	memset(pdu, 0x22U, 17U);
	pdu[0] = 0x08U;
	actions = btd_smp_input(&smp, pdu, 17U);
	memcpy(pdu, "\x09\x00\x03\x0e\x0d\x0c\x0b\x0a", 8U);
	actions = btd_smp_input(&smp, pdu, 8U);
	expect(actions == BTD_SMP_DONE && smp.keys.ltk[0] == 0x11U && smp.keys.ediv == 0x1234U && smp.keys.rand[7] == 0x08U &&
	       !smp.keys.secure && !smp.keys.authenticated,
	       "legacy: paired with the responder's LTK, EDIV and Rand");
}

/* A short key, the debug key, a wrong confirm, a failed DHKey, a responder that gives up. */
static void
test_smp_failures(void)
{
	struct btd_smp smp;
	struct peer peer;
	uint8_t key[64];
	uint8_t pdu[65];
	unsigned actions;

	/* A key of 7 bytes: Encryption Key Size. */
	btd_smp_init(&smp, BTD_SMP_DISPLAY_YES_NO, 0U, address_a, 0U, address_b, test_random, NULL);
	memset(&peer, 0, sizeof(peer));
	peer.io = 0x03U;
	peer.authentication = 0x09U;
	peer.key_size = 7U;
	(void)btd_smp_start(&smp);
	peer_response(&peer, &smp, &actions);
	expect((actions & BTD_SMP_FAILED) != 0U && smp.failure == 0x06U && smp.out[0] == 0x05U && smp.out[1] == 0x06U &&
	       smp.why == BTD_SMP_WHY_KEY_SIZE,
	       "failures: a key of 7 bytes");

	/* A key of 6 bytes is not one the Core allows: Invalid Parameters (review M13). */
	btd_smp_init(&smp, BTD_SMP_DISPLAY_YES_NO, 0U, address_a, 0U, address_b, test_random, NULL);
	peer.key_size = 6U;
	(void)btd_smp_start(&smp);
	peer_response(&peer, &smp, &actions);
	expect((actions & BTD_SMP_FAILED) != 0U && smp.failure == 0x0aU && smp.why == BTD_SMP_WHY_PROTOCOL, "failures: a key of 6 bytes");

	/* The debug key. */
	btd_smp_init(&smp, BTD_SMP_DISPLAY_YES_NO, 0U, address_a, 0U, address_b, test_random, NULL);
	peer.key_size = 16U;
	(void)btd_smp_start(&smp);
	peer_response(&peer, &smp, &actions);
	hex(key_a_x, key);
	hex(key_a_y, key + 32);
	(void)btd_smp_local_key(&smp, 0U, key);
	hex(debug_x, key);
	hex(debug_y, key + 32);
	pdu[0] = 0x0cU;
	reverse(pdu + 1, key, 32U);
	reverse(pdu + 33, key + 32, 32U);
	actions = btd_smp_input(&smp, pdu, 65U);
	expect((actions & BTD_SMP_FAILED) != 0U && (actions & BTD_SMP_DHKEY) == 0U && smp.failure == 0x08U &&
	       smp.why == BTD_SMP_WHY_DEBUG_KEY,
	       "failures: the debug key is refused (Unspecified Reason, review M1)");

	/* The controller's own key is the debug key (a controller in its debug mode). */
	btd_smp_init(&smp, BTD_SMP_DISPLAY_YES_NO, 0U, address_a, 0U, address_b, test_random, NULL);
	(void)btd_smp_start(&smp);
	peer_response(&peer, &smp, &actions);
	hex(debug_x, key);
	hex(debug_y, key + 32);
	reverse(pdu, key, 32U);
	reverse(pdu + 32, key + 32, 32U);
	actions = btd_smp_local_key(&smp, 0U, pdu);
	expect((actions & BTD_SMP_FAILED) != 0U && smp.why == BTD_SMP_WHY_DEBUG_KEY, "failures: the controller's own debug key");

	/* A responder that reflects bluetoothd's own key. */
	btd_smp_init(&smp, BTD_SMP_DISPLAY_YES_NO, 0U, address_a, 0U, address_b, test_random, NULL);
	(void)btd_smp_start(&smp);
	peer_response(&peer, &smp, &actions);
	hex(key_a_x, key);
	hex(key_a_y, key + 32);
	(void)btd_smp_local_key(&smp, 0U, key);
	pdu[0] = 0x0cU;
	memcpy(pdu + 1, key, 64U);
	actions = btd_smp_input(&smp, pdu, 65U);
	expect((actions & BTD_SMP_FAILED) != 0U && (actions & BTD_SMP_DHKEY) == 0U && smp.why == BTD_SMP_WHY_REFLECTION,
	       "failures: a reflected key is refused (review S4)");

	/* A failed DHKey (an invalid point), and a wrong confirm. */
	btd_smp_init(&smp, BTD_SMP_DISPLAY_YES_NO, 0U, address_a, 0U, address_b, test_random, NULL);
	(void)btd_smp_start(&smp);
	peer_response(&peer, &smp, &actions);
	hex(key_a_x, key);
	(void)btd_smp_local_key(&smp, 0U, key);
	hex(key_b_x, key);
	pdu[0] = 0x0cU;
	memcpy(pdu + 1, key, 64U);
	(void)btd_smp_input(&smp, pdu, 65U);
	memset(pdu, 0x33U, 17U);
	pdu[0] = 0x03U;
	(void)btd_smp_input(&smp, pdu, 17U);
	pdu[0] = 0x04U;
	actions = btd_smp_input(&smp, pdu, 17U);
	expect((actions & BTD_SMP_FAILED) != 0U && smp.failure == 0x04U && smp.why == BTD_SMP_WHY_CHECK, "failures: a wrong confirm");
	btd_smp_init(&smp, BTD_SMP_DISPLAY_YES_NO, 0U, address_a, 0U, address_b, test_random, NULL);
	(void)btd_smp_start(&smp);
	peer_response(&peer, &smp, &actions);
	(void)btd_smp_local_key(&smp, 0U, key);
	(void)btd_smp_input(&smp, pdu, 1U);
	expect(smp.failure != 0U, "failures: an empty PDU");
	btd_smp_init(&smp, BTD_SMP_DISPLAY_YES_NO, 0U, address_a, 0U, address_b, test_random, NULL);
	(void)btd_smp_start(&smp);
	peer_response(&peer, &smp, &actions);
	(void)btd_smp_local_key(&smp, 0U, key);
	pdu[0] = 0x0cU;
	(void)btd_smp_input(&smp, pdu, 65U);
	actions = btd_smp_dhkey(&smp, 0x12U, key);
	expect((actions & BTD_SMP_FAILED) != 0U && smp.failure == 0x0bU && smp.why == BTD_SMP_WHY_DHKEY, "failures: a failed DHKey");

	/* The responder gives up. */
	btd_smp_init(&smp, BTD_SMP_DISPLAY_YES_NO, 0U, address_a, 0U, address_b, test_random, NULL);
	(void)btd_smp_start(&smp);
	actions = btd_smp_input(&smp, (const uint8_t *)"\x05\x03", 2U);
	expect(actions == BTD_SMP_FAILED && smp.failure == 0x03U && smp.why == BTD_SMP_WHY_REJECTED, "failures: Pairing Failed from the responder");
}

/* A bond written, read and forgotten; broken files. */
static void
test_keys(void)
{
	struct btd_bond bond;
	struct btd_bond read_back;
	char text[1024];
	uint8_t address[6];
	int error;

	/* A BR/EDR bond and an LE one. */
	memset(&bond, 0, sizeof(bond));
	memcpy(bond.address, address_b, 6U);
	bond.type = 0U;
	(void)snprintf(bond.name, sizeof(bond.name), "%s", "Kei \"keyboard\"");
	bond.have_link_key = 1;
	memset(bond.link_key, 0xa5U, 16U);
	bond.link_key_type = 5U;
	bond.authenticated = 1;
	error = btd_keys_write(keys_folder, address_a, &bond);
	expect(error == 0, "keys: written (%d)", error);
	error = btd_keys_read(keys_folder, address_a, address_b, 0U, &read_back);
	expect(error == 0 && read_back.have_link_key && read_back.link_key[15] == 0xa5U && read_back.link_key_type == 5U &&
	       read_back.authenticated && strcmp(read_back.name, "Kei \\x22keyboard\\x22") == 0,
	       "keys: read back (%d, %s)", error, read_back.name);
	memset(&bond, 0, sizeof(bond));
	memcpy(bond.address, address_b, 6U);
	bond.type = 1U;
	bond.have_ltk = 1;
	memset(bond.ltk, 0x3cU, 16U);
	bond.ediv = 0x1234U;
	bond.key_size = 16U;
	bond.secure = 1;
	bond.have_irk = 1;
	bond.have_identity = 1;
	memcpy(bond.identity, address_b, 6U);
	error = btd_keys_write(keys_folder, address_a, &bond);
	error = btd_keys_read(keys_folder, address_a, address_b, 1U, &read_back);
	expect(error == 0 && read_back.have_ltk && read_back.ediv == 0x1234U && read_back.secure && read_back.have_identity &&
	       memcmp(read_back.identity, address_b, 6U) == 0,
	       "keys: an LE bond read back");

	/* Forgotten; another type is another file. */
	error = btd_keys_forget(keys_folder, address_a, address_b, 1U);
	expect(error == 0, "keys: forgotten");
	error = btd_keys_read(keys_folder, address_a, address_b, 1U, &read_back);
	expect(error == ENOENT, "keys: gone");
	error = btd_keys_read(keys_folder, address_a, address_b, 0U, &read_back);
	expect(error == 0, "keys: the BR/EDR bond stays");

	/* Broken texts: no type, a short key, a line without '=', an unknown key is passed over. */
	error = btd_keys_parse("name=x\n", 7U, &read_back);
	expect(error == EBADMSG, "keys: no type");
	(void)snprintf(text, sizeof(text), "%s", "type=bredr\nlink_key=00\n");
	error = btd_keys_parse(text, strlen(text), &read_back);
	expect(error == EBADMSG, "keys: a short key");
	(void)snprintf(text, sizeof(text), "%s", "type=bredr\nnonsense\n");
	error = btd_keys_parse(text, strlen(text), &read_back);
	expect(error == EBADMSG, "keys: a line without '='");
	(void)snprintf(text, sizeof(text), "%s", "type=le-random\nfuture=1\n");
	error = btd_keys_parse(text, strlen(text), &read_back);
	expect(error == 0 && read_back.type == 2U, "keys: an unknown key is passed over");

	/* Addresses. */
	error = btd_address_parse("0A:0B:0C:0D:0E:03", address);
	expect(error == 0 && memcmp(address, address_b, 6U) == 0, "keys: an address read");
	error = btd_address_parse("0A:0B:0C:0D:0E:0G", address);
	expect(error == EINVAL, "keys: a broken address");
	error = btd_address_parse("0A:0B:0C:0D:0E", address);
	expect(error == EINVAL, "keys: a short address");
}

/* Random bytes into the parsers; ASan and UBSan watch. */
static void
test_fuzz(void)
{
	struct btd_reassembly reassembly;
	struct btd_signal_effect effect;
	struct btd_l2cap l2cap;
	struct btd_bond bond;
	struct btd_smp smp;
	struct btd_acl acl;
	uint8_t bytes[300];
	uint8_t answer[BTD_SIGNAL_MAX];
	char text[300];
	size_t length;
	size_t answer_length;
	unsigned round;
	unsigned index;
	int parsed;

	/* A fixed seed: the same run every time. */
	srand(144U);
	memset(&reassembly, 0, sizeof(reassembly));
	btd_l2cap_init(&l2cap);
	for (round = 0U; round < TEST_FUZZ_ROUNDS; round++) {
		/* Random bytes. */
		length = (size_t)(rand() % (int)sizeof(bytes));
		for (index = 0U; index < length; index++)
			bytes[index] = (uint8_t)rand();

		/* ACL and its frames. */
		if (length > 0U)
			bytes[0] = 0x02U;
		parsed = btd_acl_parse(bytes, length, &acl);
		if (parsed == 0)
			(void)btd_reassembly_feed(&reassembly, &acl);
		acl.boundary = (uint8_t)(round & 3U);
		acl.data = bytes;
		acl.length = length;
		(void)btd_reassembly_feed(&reassembly, &acl);

		/* L2CAP's signalling, both kinds. */
		(void)btd_l2cap_signal(&l2cap, 0x0040U, (int)(round & 1U), bytes, length, answer, sizeof(answer), &answer_length, &effect);

		/* SMP at each state the random PDUs reach. */
		if ((round % 64U) == 0U) {
			btd_smp_init(&smp, BTD_SMP_DISPLAY_YES_NO, 0U, address_a, 0U, address_b, test_random, NULL);
			(void)btd_smp_start(&smp);
		}

		/* The random PDU, and a random key from the controller. */
		(void)btd_smp_input(&smp, bytes, length);
		(void)btd_smp_local_key(&smp, 0U, bytes);

		/* Bond texts. */
		memcpy(text, bytes, length);
		text[length] = '\0';
		(void)btd_keys_parse(text, length, &bond);
	}

	/* The fuzz ran to its end. */
	expect(1, "fuzz");
	printf("bt-pair-host-test: fuzz %u rounds\n", TEST_FUZZ_ROUNDS);
}
