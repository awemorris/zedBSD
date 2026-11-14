/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The test of ssh-kex.c.  The KEXINIT payloads a peer sends are built here
 * with wire.h from RFC 4253 section 7.1 (message number 20, a 16-byte
 * cookie, ten name-lists, first_kex_packet_follows, a reserved uint32), apart
 * from the module.  The markers follow draft-ietf-sshm-strict-kex-02
 * section 3.1, the messages draft-ietf-sshm-mlkem-hybrid-kex-10 section 2.2
 * (30 with one string; 31 with three strings), and the exchange hash H its
 * section 2.5 (eight strings, K as a string of 32 bytes).  H and the key
 * derivation (RFC 4253 section 7.2) are rebuilt here from wire.h and
 * ssh_sha256, and checked against values computed with Python's hashlib over
 * the same concatenation (the known-answer values below).  The OpenSSH 10.5
 * default name-lists are written out from the "default is" blocks of
 * openssh-10.5p1-ssh_config.5 (KexAlgorithms, HostKeyAlgorithms, Ciphers,
 * MACs).
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "wire.h"
#include "ssh-kex.h"

/* The number of random inputs of the robustness checks. */
#define TEST_RANDOM_RUNS	100000U

/* The number of name-lists of a KEXINIT. */
#define TEST_LISTS		10U

/* One wire buffer for the payloads the test builds (large: static). */
static struct wire payload_wire;
static struct wire second_wire;

/* The state of the pseudo random generator that makes the random inputs. */
static uint32_t random_state = 0x2545f491U;

/*
 * Values referenced: openssh-10.5p1-ssh_config.5, the "The default is"
 * blocks of KexAlgorithms, HostKeyAlgorithms, Ciphers and MACs, with the
 * names OpenSSH adds to the kex list in its KEXINIT ("ext-info-c" and the
 * pre-standard strict marker).
 */
static const char openssh_kex[] =
	"mlkem768x25519-sha256,sntrup761x25519-sha512,sntrup761x25519-sha512@openssh.com,"
	"curve25519-sha256,curve25519-sha256@libssh.org,"
	"ecdh-sha2-nistp256,ecdh-sha2-nistp384,ecdh-sha2-nistp521,"
	"diffie-hellman-group-exchange-sha256,diffie-hellman-group16-sha512,"
	"diffie-hellman-group18-sha512,diffie-hellman-group14-sha256,"
	"ext-info-c,kex-strict-c-v00@openssh.com";
static const char openssh_hostkey[] =
	"ssh-ed25519-cert-v01@openssh.com,ecdsa-sha2-nistp256-cert-v01@openssh.com,"
	"ecdsa-sha2-nistp384-cert-v01@openssh.com,ecdsa-sha2-nistp521-cert-v01@openssh.com,"
	"sk-ssh-ed25519-cert-v01@openssh.com,sk-ecdsa-sha2-nistp256-cert-v01@openssh.com,"
	"webauthn-sk-ecdsa-sha2-nistp256-cert-v01@openssh.com,rsa-sha2-512-cert-v01@openssh.com,"
	"rsa-sha2-256-cert-v01@openssh.com,ssh-mldsa44-ed25519-cert-v01@openssh.com,"
	"ssh-ed25519,ecdsa-sha2-nistp256,ecdsa-sha2-nistp384,ecdsa-sha2-nistp521,"
	"sk-ecdsa-sha2-nistp256@openssh.com,webauthn-sk-ecdsa-sha2-nistp256@openssh.com,"
	"sk-ssh-ed25519@openssh.com,rsa-sha2-512,rsa-sha2-256,ssh-mldsa44-ed25519@openssh.com";
static const char openssh_cipher[] =
	"chacha20-poly1305@openssh.com,aes128-gcm@openssh.com,aes256-gcm@openssh.com,"
	"aes128-ctr,aes192-ctr,aes256-ctr";
static const char openssh_mac[] =
	"umac-64-etm@openssh.com,umac-128-etm@openssh.com,hmac-sha2-256-etm@openssh.com,"
	"hmac-sha2-512-etm@openssh.com,hmac-sha1-etm@openssh.com,umac-64@openssh.com,"
	"umac-128@openssh.com,hmac-sha2-256,hmac-sha2-512,hmac-sha1";

/*
 * The known-answer values: the exchange hash of the inputs make_input
 * builds, and the key material of derive_inputs.  Computed with Python's
 * hashlib over the strings of draft-ietf-sshm-mlkem-hybrid-kex-10 2.5 and the
 * blocks of RFC 4253 7.2 (K as uint32 32 and 32 bytes).
 *
 * Values referenced: draft-ietf-sshm-mlkem-hybrid-kex-10 section 2.5;
 * RFC 4253 section 7.2.
 */
static const char kat_hash[] =
	"df10c152954e0e820a0a285fd2810f3f288ccca1641da8fcb3529951f7d02daa";
static const char kat_derive_c[] =
	"c4ce46c651ff2c150f783e401b86a74f1f3fec2cdff2dc4ba4b2a47ddab65cb3"
	"eeffc44d74aea2c5ac67857bc0d2c89aec83d255f21f7f486004715d4c9b181b";
static const char kat_derive_d[] =
	"c1abd8779ff4e7a9ca3558572a3eda5e66ba1749852f19e31206fa633ef5b835"
	"fb293a3b8816d91a56d10cd687c6f52001949d8c1fcf698051efd8df04416940";
static const char kat_derive_a[] =
	"3236d06516c6ff9030f5fa8bd794f376d50392f70da59bc9a9c1b73928939b59"
	"dc1b6035f06e03e759b2f9a67855b40d6e13e89a88c1d57584194c75f9a4de14"
	"cf1b87e6afcc8f2c548e264e2f5d1c28d38857f05603ec6f2ad354a6fb2537b0"
	"79d0e07d7aaf60fb87996fb946d55685db26368c20c102477afbc36b50971b91"
	"75566617e9905aa2501c0c134580f3527b37dde996f97074566b1229cf481fa6"
	"a21aa3e5715ec2b55e48cc5f62485be5a95554f618093002c6876c8b7fb58479"
	"56cefb7092974c30320317f826b6c74f2427674f4239942cea5f31dcdc6fa7df"
	"e273aec2c10d50ec233e83331e7ead3109c12f5fa574a1872337153b79cdea0a";
static const char kat_derive_f33[] =
	"d7a6b2e3cf2ba9b7fe0feae8141f3d789ae9c6847754189a72cf15dd5864170aba";

/* The fields of the exchange hash input the known-answer test uses (patterned bytes). */
static uint8_t in_kexinit_client[37];
static uint8_t in_kexinit_server[41];
static uint8_t in_host_key[51];
static uint8_t in_c_init[SSH_HYBRID_INIT_SIZE];
static uint8_t in_s_reply[SSH_HYBRID_REPLY_SIZE];
static uint8_t in_k[32];
static const char in_version_client[] = "SSH-2.0-zedBSD_1.0";
static const char in_version_server[] = "SSH-2.0-OpenSSH_10.5";

static void test_put(void);
static void test_put_errors(void);
static void test_parse_roundtrip(void);
static void test_parse_openssh(void);
static void test_parse_errors(void);
static void test_parse_truncations(void);
static void test_choose_missing(void);
static void test_choose_strict(void);
static void test_choose_guess(void);
static void test_choose_openssh(void);
static void test_hybrid_init(void);
static void test_hybrid_reply(void);
static void test_hybrid_errors(void);
static void test_hash(void);
static void test_hash_sensitivity(void);
static void test_derive(void);
static void test_derive_errors(void);
static void test_random_feed(void);
static uint32_t next_random(void);
static void fill_pattern(uint8_t *bytes, size_t size, unsigned multiplier, unsigned offset);
static void make_input(struct ssh_kex_hash_input *input);
static void build_kexinit(struct wire *wire, const char *const *lists, unsigned first, uint32_t reserved);
static void build_standard_lists(const char **lists, const char *kex);
static void parse_ok(const uint8_t *payload, size_t size, struct ssh_kexinit *kexinit, int expected);
static int parse_exact(const uint8_t *payload, size_t size);
static void expect_hex(const uint8_t *actual, const char *hex, size_t size, const char *what);

/* Runs every check of the key exchange module and prints the last line. */
int
main(void)
{
	/* Runs each group of checks. */
	test_put();
	test_put_errors();
	test_parse_roundtrip();
	test_parse_openssh();
	test_parse_errors();
	test_parse_truncations();
	test_choose_missing();
	test_choose_strict();
	test_choose_guess();
	test_choose_openssh();
	test_hybrid_init();
	test_hybrid_reply();
	test_hybrid_errors();
	test_hash();
	test_hash_sensitivity();
	test_derive();
	test_derive_errors();
	test_random_feed();

	/* Prints the verdict. */
	return check_finish("kex");
}

/* Returns the next number of a xorshift generator. */
static uint32_t
next_random(void)
{
	/* One xorshift step. */
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;
	return random_state;
}

/* Fills size bytes with byte[i] = (i * multiplier + offset) mod 256. */
static void
fill_pattern(
	uint8_t *bytes,
	size_t size,
	unsigned multiplier,
	unsigned offset)
{
	size_t index;

	/* Each byte depends on its place. */
	for (index = 0; index < size; index++)
		bytes[index] = (uint8_t)((index * multiplier + offset) & 0xffU);
}

/* Builds the inputs of the exchange hash known-answer test. */
static void
make_input(
	struct ssh_kex_hash_input *input)
{
	/* Fills the patterned fields. */
	fill_pattern(in_kexinit_client, sizeof(in_kexinit_client), 7, 1);
	fill_pattern(in_kexinit_server, sizeof(in_kexinit_server), 11, 2);
	fill_pattern(in_host_key, sizeof(in_host_key), 13, 3);
	fill_pattern(in_c_init, sizeof(in_c_init), 5, 4);
	fill_pattern(in_s_reply, sizeof(in_s_reply), 3, 5);
	fill_pattern(in_k, sizeof(in_k), 17, 6);

	/* Points the input at them. */
	input->version_client = (const uint8_t *)in_version_client;
	input->version_client_size = strlen(in_version_client);
	input->version_server = (const uint8_t *)in_version_server;
	input->version_server_size = strlen(in_version_server);
	input->kexinit_client = in_kexinit_client;
	input->kexinit_client_size = sizeof(in_kexinit_client);
	input->kexinit_server = in_kexinit_server;
	input->kexinit_server_size = sizeof(in_kexinit_server);
	input->host_key = in_host_key;
	input->host_key_size = sizeof(in_host_key);
	input->c_init = in_c_init;
	input->c_init_size = sizeof(in_c_init);
	input->s_reply = in_s_reply;
	input->s_reply_size = sizeof(in_s_reply);
	input->k = in_k;
}

/*
 * Builds a KEXINIT payload with wire.h: the message number, a cookie of
 * bytes 0xc0 + index, the ten lists (NUL-terminated texts), the boolean
 * given and the reserved uint32 given.
 */
static void
build_kexinit(
	struct wire *wire,
	const char *const *lists,
	unsigned first,
	uint32_t reserved)
{
	unsigned index;

	/* Writes the fields in the order of RFC 4253 7.1. */
	wire_reset(wire);
	wire_u8(wire, 20);
	for (index = 0; index < 16; index++)
		wire_u8(wire, 0xc0U + index);
	for (index = 0; index < TEST_LISTS; index++)
		wire_text(wire, lists[index]);
	wire_bool(wire, first);
	wire_u32(wire, reserved);
}

/* Fills the ten lists with the names these programs accept, with the given kex list. */
static void
build_standard_lists(
	const char **lists,
	const char *kex)
{
	/* The order of RFC 4253 7.1. */
	lists[0] = kex;
	lists[1] = "ssh-ed25519";
	lists[2] = "chacha20-poly1305@openssh.com";
	lists[3] = "chacha20-poly1305@openssh.com";
	lists[4] = "";
	lists[5] = "";
	lists[6] = "none";
	lists[7] = "none";
	lists[8] = "";
	lists[9] = "";
}

/* Parses a payload kept in memory that stays valid, and checks the result code. */
static void
parse_ok(
	const uint8_t *payload,
	size_t size,
	struct ssh_kexinit *kexinit,
	int expected)
{
	int error;

	/* Parses it. */
	error = ssh_kexinit_parse(payload, size, kexinit);
	CHECK_EQ(error, expected);
}

/*
 * Parses a copy of the payload held in memory of exactly its size, so that
 * a read past the end is caught; returns the error code.
 */
static int
parse_exact(
	const uint8_t *payload,
	size_t size)
{
	struct ssh_kexinit kexinit;
	uint8_t *copy;
	int error;

	/* Copies the payload into memory of exactly its size. */
	copy = malloc(size == 0 ? 1 : size);
	if (copy == NULL)
		abort();
	if (size > 0)
		memcpy(copy, payload, size);

	/* Parses it and frees the copy. */
	error = ssh_kexinit_parse(copy, size, &kexinit);
	free(copy);
	return error;
}

/* Compares size bytes with a hexadecimal text. */
static void
expect_hex(
	const uint8_t *actual,
	const char *hex,
	size_t size,
	const char *what)
{
	uint8_t expected[512];
	size_t count;

	/* Decodes the expected value and compares. */
	count = wire_hex(hex, expected, sizeof(expected));
	CHECK_EQ(count, size);
	if (count == size)
		check_record_bytes(actual, expected, size, __FILE__, __LINE__, what);
}

/* Checks what ssh_kexinit_put writes for each side and exchange against bytes built with wire.h. */
static void
test_put(void)
{
	static const char *const expect_kex[2][2] = {
		/* client: later exchange, initial exchange; server: likewise. */
		{ "mlkem768x25519-sha256",
		  "mlkem768x25519-sha256,kex-strict-c,kex-strict-c-v00@openssh.com" },
		{ "mlkem768x25519-sha256",
		  "mlkem768x25519-sha256,kex-strict-s,kex-strict-s-v00@openssh.com" }
	};
	struct ssh_buf out;
	struct ssh_buf again;
	const char *lists[TEST_LISTS];
	int server;
	int initial;
	int error;

	/* Checks each of the four combinations. */
	for (server = 0; server < 2; server++) {
		for (initial = 0; initial < 2; initial++) {
			ssh_buf_init(&out, 4096, 0);
			error = ssh_kexinit_put(server, initial, &out);
			CHECK_EQ(error, 0);

			/* The expected bytes: the cookie is random, so its place is copied over. */
			build_standard_lists(lists, expect_kex[server][initial]);
			build_kexinit(&payload_wire, lists, 0, 0);
			CHECK_EQ(out.size, payload_wire.size);
			if (out.size == payload_wire.size) {
				CHECK_EQ(out.data[0], 20);
				CHECK_BYTES(out.data + 17, payload_wire.data + 17, out.size - 17);
			}

			/* A second KEXINIT has a different cookie (16 random bytes). */
			ssh_buf_init(&again, 4096, 0);
			error = ssh_kexinit_put(server, initial, &again);
			CHECK_EQ(error, 0);
			CHECK(again.size == out.size);
			CHECK(memcmp(again.data + 1, out.data + 1, 16) != 0);
			ssh_buf_free(&again);
			ssh_buf_free(&out);
		}
	}
}

/* Checks the errors of ssh_kexinit_put. */
static void
test_put_errors(void)
{
	struct ssh_buf out;
	size_t exact;
	int error;

	/* A buffer whose limit is too small records EMSGSIZE and the call reports it. */
	ssh_buf_init(&out, 100, 0);
	error = ssh_kexinit_put(1, 1, &out);
	CHECK_EQ(error, EMSGSIZE);
	ssh_buf_free(&out);

	/* A buffer that has already failed keeps its error and gets nothing more. */
	ssh_buf_init(&out, 10, 0);
	ssh_buf_put_bytes(&out, "0123456789ab", 12);
	CHECK_EQ(ssh_buf_error(&out), EMSGSIZE);
	error = ssh_kexinit_put(0, 0, &out);
	CHECK_EQ(error, EMSGSIZE);
	CHECK_EQ(out.size, 0);
	ssh_buf_free(&out);

	/* A limit just large enough succeeds; one byte less fails. */
	ssh_buf_init(&out, 4096, 0);
	error = ssh_kexinit_put(0, 0, &out);
	CHECK_EQ(error, 0);
	exact = out.size;
	ssh_buf_free(&out);
	ssh_buf_init(&out, exact, 0);
	error = ssh_kexinit_put(0, 0, &out);
	CHECK_EQ(error, 0);
	ssh_buf_free(&out);
	ssh_buf_init(&out, exact - 1, 0);
	error = ssh_kexinit_put(0, 0, &out);
	CHECK_EQ(error, EMSGSIZE);
	ssh_buf_free(&out);
}

/* Parses what ssh_kexinit_put writes and checks each list. */
static void
test_parse_roundtrip(void)
{
	static const char *const expect_kex[2][2] = {
		{ "mlkem768x25519-sha256",
		  "mlkem768x25519-sha256,kex-strict-c,kex-strict-c-v00@openssh.com" },
		{ "mlkem768x25519-sha256",
		  "mlkem768x25519-sha256,kex-strict-s,kex-strict-s-v00@openssh.com" }
	};
	static uint8_t kept[1024];
	struct ssh_buf out;
	struct ssh_kexinit kexinit;
	const char *lists[TEST_LISTS];
	unsigned index;
	int server;
	int initial;
	int error;

	/* Checks each of the four combinations. */
	for (server = 0; server < 2; server++) {
		for (initial = 0; initial < 2; initial++) {
			ssh_buf_init(&out, 4096, 0);
			error = ssh_kexinit_put(server, initial, &out);
			CHECK_EQ(error, 0);
			memcpy(kept, out.data, out.size);
			parse_ok(kept, out.size, &kexinit, 0);
			build_standard_lists(lists, expect_kex[server][initial]);

			/* Every list is the one expected and lies inside the payload. */
			for (index = 0; index < TEST_LISTS; index++) {
				CHECK_EQ(kexinit.list_size[index], strlen(lists[index]));
				CHECK_BYTES(kexinit.list[index], lists[index], strlen(lists[index]));
				CHECK((const uint8_t *)kexinit.list[index] >= kept);
				CHECK((const uint8_t *)kexinit.list[index] + kexinit.list_size[index] <= kept + out.size);
			}

			CHECK_EQ(kexinit.first_kex_packet_follows, 0);
			ssh_buf_free(&out);
		}
	}
}

/* Parses a KEXINIT with the OpenSSH 10.5 default lists and the flag set. */
static void
test_parse_openssh(void)
{
	static uint8_t kept[WIRE_MAX];
	struct ssh_kexinit kexinit;
	const char *lists[TEST_LISTS];

	/* The lists of a client in the order of RFC 4253 7.1; compression "none,zlib@openssh.com". */
	lists[0] = openssh_kex;
	lists[1] = openssh_hostkey;
	lists[2] = openssh_cipher;
	lists[3] = openssh_cipher;
	lists[4] = openssh_mac;
	lists[5] = openssh_mac;
	lists[6] = "none,zlib@openssh.com";
	lists[7] = "none,zlib@openssh.com";
	lists[8] = "";
	lists[9] = "";
	build_kexinit(&payload_wire, lists, 0, 0);
	memcpy(kept, payload_wire.data, payload_wire.size);
	parse_ok(kept, payload_wire.size, &kexinit, 0);
	CHECK_EQ(kexinit.list_size[0], strlen(openssh_kex));
	CHECK_BYTES(kexinit.list[0], openssh_kex, strlen(openssh_kex));
	CHECK_EQ(kexinit.list_size[1], strlen(openssh_hostkey));
	CHECK_EQ(kexinit.list_size[4], strlen(openssh_mac));
	CHECK_EQ(kexinit.list_size[8], 0);
	CHECK_EQ(kexinit.first_kex_packet_follows, 0);

	/* The flag is read as a boolean: any non-zero byte is TRUE; the reserved field may carry any value. */
	build_kexinit(&payload_wire, lists, 1, 0);
	memcpy(kept, payload_wire.data, payload_wire.size);
	parse_ok(kept, payload_wire.size, &kexinit, 0);
	CHECK_EQ(kexinit.first_kex_packet_follows, 1);
	build_kexinit(&payload_wire, lists, 2, 0xdeadbeefU);
	memcpy(kept, payload_wire.data, payload_wire.size);
	parse_ok(kept, payload_wire.size, &kexinit, 0);
	CHECK_EQ(kexinit.first_kex_packet_follows, 1);
}

/* Checks the malformed payloads: a wrong number, bad lists, trailing bytes, a payload too long. */
static void
test_parse_errors(void)
{
	static uint8_t kept[SSH_KEXINIT_MAX + 64U];
	static char longlist[8300];
	struct ssh_kexinit kexinit;
	const char *lists[TEST_LISTS];
	size_t index;
	int error;

	build_standard_lists(lists, "mlkem768x25519-sha256");

	/* A payload of another message number. */
	build_kexinit(&payload_wire, lists, 0, 0);
	payload_wire.data[0] = 21;
	CHECK_EQ(parse_exact(payload_wire.data, payload_wire.size), EINVAL);

	/* A trailing byte. */
	build_kexinit(&payload_wire, lists, 0, 0);
	wire_u8(&payload_wire, 0);
	CHECK_EQ(parse_exact(payload_wire.data, payload_wire.size), EINVAL);

	/* A list with an empty name, a leading comma, a trailing comma, a space, a high byte. */
	lists[0] = "a,,b";
	build_kexinit(&payload_wire, lists, 0, 0);
	CHECK_EQ(parse_exact(payload_wire.data, payload_wire.size), EINVAL);
	lists[0] = ",a";
	build_kexinit(&payload_wire, lists, 0, 0);
	CHECK_EQ(parse_exact(payload_wire.data, payload_wire.size), EINVAL);
	lists[0] = "a,";
	build_kexinit(&payload_wire, lists, 0, 0);
	CHECK_EQ(parse_exact(payload_wire.data, payload_wire.size), EINVAL);
	lists[0] = "a b";
	build_kexinit(&payload_wire, lists, 0, 0);
	CHECK_EQ(parse_exact(payload_wire.data, payload_wire.size), EINVAL);
	lists[0] = "a\xc3\xa9";
	build_kexinit(&payload_wire, lists, 0, 0);
	CHECK_EQ(parse_exact(payload_wire.data, payload_wire.size), EINVAL);

	/* A bad list in each of the other places is also refused (the last list too). */
	for (index = 1; index < TEST_LISTS; index++) {
		build_standard_lists(lists, "mlkem768x25519-sha256");
		lists[index] = "x,,y";
		build_kexinit(&payload_wire, lists, 0, 0);
		CHECK_EQ(parse_exact(payload_wire.data, payload_wire.size), EINVAL);
	}

	/* A name-list longer than SSH_NAMELIST_MAX (8193 bytes, valid names) is EMSGSIZE. */
	longlist[0] = 'a';
	for (index = 1; index < 8193; index += 2) {
		longlist[index] = ',';
		longlist[index + 1] = 'a';
	}

	longlist[8193] = '\0';
	build_standard_lists(lists, "mlkem768x25519-sha256");
	lists[9] = longlist;
	build_kexinit(&payload_wire, lists, 0, 0);
	CHECK_EQ(parse_exact(payload_wire.data, payload_wire.size), EMSGSIZE);

	/* A list of 8192 bytes ends in a comma here: a trailing comma is refused. */
	longlist[8192] = '\0';
	lists[9] = longlist;
	build_kexinit(&payload_wire, lists, 0, 0);
	CHECK_EQ(parse_exact(payload_wire.data, payload_wire.size), EINVAL);

	/* A list of exactly SSH_NAMELIST_MAX (8192) bytes passes when it is well formed (the last name has two letters). */
	longlist[8191] = 'a';
	build_kexinit(&payload_wire, lists, 0, 0);
	CHECK_EQ(parse_exact(payload_wire.data, payload_wire.size), 0);

	/* A payload longer than SSH_KEXINIT_MAX is EMSGSIZE, whatever its bytes; exactly the limit is parsed. */
	memset(kept, 0, sizeof(kept));
	kept[0] = 20;
	error = ssh_kexinit_parse(kept, SSH_KEXINIT_MAX + 1U, &kexinit);
	CHECK_EQ(error, EMSGSIZE);
	error = ssh_kexinit_parse(kept, SSH_KEXINIT_MAX, &kexinit);
	CHECK_EQ(error, EINVAL);

	/* An error leaves the structure empty. */
	memset(&kexinit, 0xff, sizeof(kexinit));
	error = ssh_kexinit_parse(NULL, 0, &kexinit);
	CHECK_EQ(error, EINVAL);
	CHECK(kexinit.list[0] == NULL);
	CHECK_EQ(kexinit.list_size[0], 0);
	CHECK_EQ(kexinit.first_kex_packet_follows, 0);
}

/* Cuts a valid payload at every length: each shorter payload is refused. */
static void
test_parse_truncations(void)
{
	const char *lists[TEST_LISTS];
	size_t length;

	build_standard_lists(lists, "mlkem768x25519-sha256,kex-strict-c");
	build_kexinit(&payload_wire, lists, 1, 7);
	CHECK_EQ(parse_exact(payload_wire.data, payload_wire.size), 0);

	/* Every proper prefix breaks a field. */
	for (length = 0; length < payload_wire.size; length++)
		CHECK_EQ(parse_exact(payload_wire.data, length), EINVAL);
}

/* Fills a parsed KEXINIT from lists (pointing into the caller's strings). */
static void
set_lists(
	struct ssh_kexinit *kexinit,
	const char *const *lists,
	int first)
{
	unsigned index;

	/* Points each list at its text. */
	memset(kexinit, 0, sizeof(*kexinit));
	for (index = 0; index < TEST_LISTS; index++) {
		kexinit->list[index] = lists[index];
		kexinit->list_size[index] = strlen(lists[index]);
	}

	kexinit->first_kex_packet_follows = first;
}

/* Checks the kinds of failure: a name the peer lacks in each of the five kinds. */
static void
test_choose_missing(void)
{
	struct ssh_kexinit peer;
	struct ssh_kex_choice choice;
	const char *lists[TEST_LISTS];
	const char *kind;
	int error;
	int server;

	/* A peer that offers everything is accepted, from either side. */
	for (server = 0; server < 2; server++) {
		build_standard_lists(lists, server ? "mlkem768x25519-sha256,kex-strict-c" : "mlkem768x25519-sha256,kex-strict-s");
		set_lists(&peer, lists, 0);
		memset(&choice, 0, sizeof(choice));
		kind = "unset";
		error = ssh_kex_choose(server, 1, &peer, &choice, &kind);
		CHECK_EQ(error, 0);
		CHECK_EQ(choice.strict, 1);
		CHECK_EQ(choice.ignore_next, 0);
		CHECK(kind == NULL);
	}

	/* No key exchange method. */
	build_standard_lists(lists, "curve25519-sha256,kex-strict-c");
	set_lists(&peer, lists, 0);
	memset(&choice, 0, sizeof(choice));
	error = ssh_kex_choose(1, 1, &peer, &choice, &kind);
	CHECK_EQ(error, ENOTSUP);
	CHECK(strstr(kind, "kex") != NULL);
	CHECK_EQ(choice.strict, 0);

	/* A name that only begins like ours does not count. */
	build_standard_lists(lists, "mlkem768x25519-sha256x,kex-strict-c");
	set_lists(&peer, lists, 0);
	error = ssh_kex_choose(1, 1, &peer, &choice, &kind);
	CHECK_EQ(error, ENOTSUP);

	/* No host key algorithm. */
	build_standard_lists(lists, "mlkem768x25519-sha256,kex-strict-c");
	lists[1] = "ssh-rsa,ecdsa-sha2-nistp256";
	set_lists(&peer, lists, 0);
	error = ssh_kex_choose(1, 1, &peer, &choice, &kind);
	CHECK_EQ(error, ENOTSUP);
	CHECK(strstr(kind, "host_key") != NULL);

	/* No cipher in one direction, then in the other. */
	build_standard_lists(lists, "mlkem768x25519-sha256,kex-strict-c");
	lists[2] = "aes128-gcm@openssh.com";
	set_lists(&peer, lists, 0);
	error = ssh_kex_choose(1, 1, &peer, &choice, &kind);
	CHECK_EQ(error, ENOTSUP);
	CHECK(strstr(kind, "encryption") != NULL);
	CHECK(strstr(kind, "client_to_server") != NULL);
	build_standard_lists(lists, "mlkem768x25519-sha256,kex-strict-c");
	lists[3] = "";
	set_lists(&peer, lists, 0);
	error = ssh_kex_choose(1, 1, &peer, &choice, &kind);
	CHECK_EQ(error, ENOTSUP);
	CHECK(strstr(kind, "server_to_client") != NULL);

	/* No "none" compression in one direction, then in the other. */
	build_standard_lists(lists, "mlkem768x25519-sha256,kex-strict-c");
	lists[6] = "zlib@openssh.com";
	set_lists(&peer, lists, 0);
	error = ssh_kex_choose(1, 1, &peer, &choice, &kind);
	CHECK_EQ(error, ENOTSUP);
	CHECK(strstr(kind, "compression") != NULL);
	CHECK(strstr(kind, "client_to_server") != NULL);
	build_standard_lists(lists, "mlkem768x25519-sha256,kex-strict-c");
	lists[7] = "zlib@openssh.com";
	set_lists(&peer, lists, 0);
	error = ssh_kex_choose(1, 1, &peer, &choice, &kind);
	CHECK_EQ(error, ENOTSUP);
	CHECK(strstr(kind, "server_to_client") != NULL);

	/* The MAC lists are not looked at: any content, also hostile, is accepted. */
	build_standard_lists(lists, "mlkem768x25519-sha256,kex-strict-c");
	lists[4] = "hmac-sha2-256";
	lists[5] = "nonsense";
	lists[8] = "ja";
	set_lists(&peer, lists, 0);
	error = ssh_kex_choose(1, 1, &peer, &choice, &kind);
	CHECK_EQ(error, 0);

	/* A NULL kind is allowed, on success and on failure. */
	error = ssh_kex_choose(1, 1, &peer, &choice, NULL);
	CHECK_EQ(error, 0);
	lists[1] = "ssh-rsa";
	set_lists(&peer, lists, 0);
	error = ssh_kex_choose(1, 1, &peer, &choice, NULL);
	CHECK_EQ(error, ENOTSUP);
}

/* Checks the four cases of the strict markers (standard, pre-standard, both, none), both sides, and the later exchange. */
static void
test_choose_strict(void)
{
	static const char *const client_kex[4] = {
		"mlkem768x25519-sha256,kex-strict-c",
		"mlkem768x25519-sha256,kex-strict-c-v00@openssh.com",
		"mlkem768x25519-sha256,kex-strict-c,kex-strict-c-v00@openssh.com",
		"mlkem768x25519-sha256"
	};
	static const char *const server_kex[4] = {
		"mlkem768x25519-sha256,kex-strict-s",
		"mlkem768x25519-sha256,kex-strict-s-v00@openssh.com",
		"mlkem768x25519-sha256,kex-strict-s-v00@openssh.com,kex-strict-s",
		"mlkem768x25519-sha256"
	};
	struct ssh_kexinit peer;
	struct ssh_kex_choice choice;
	const char *lists[TEST_LISTS];
	const char *kind;
	unsigned index;
	int error;

	/* This side is the server: the peer's markers are the client's. */
	for (index = 0; index < 4; index++) {
		build_standard_lists(lists, client_kex[index]);
		set_lists(&peer, lists, 0);
		memset(&choice, 0, sizeof(choice));
		kind = NULL;
		error = ssh_kex_choose(1, 1, &peer, &choice, &kind);
		if (index < 3) {
			CHECK_EQ(error, 0);
			CHECK_EQ(choice.strict, 1);
		} else {
			CHECK_EQ(error, EPROTO);
			CHECK_EQ(choice.strict, 0);
			CHECK(kind != NULL);
		}
	}

	/* This side is the client: the peer's markers are the server's. */
	for (index = 0; index < 4; index++) {
		build_standard_lists(lists, server_kex[index]);
		set_lists(&peer, lists, 0);
		memset(&choice, 0, sizeof(choice));
		error = ssh_kex_choose(0, 1, &peer, &choice, NULL);
		if (index < 3) {
			CHECK_EQ(error, 0);
			CHECK_EQ(choice.strict, 1);
		} else {
			CHECK_EQ(error, EPROTO);
		}
	}

	/* A marker of the wrong side does not count: the server offering the client's marker, and the reverse. */
	build_standard_lists(lists, "mlkem768x25519-sha256,kex-strict-c");
	set_lists(&peer, lists, 0);
	error = ssh_kex_choose(0, 1, &peer, &choice, NULL);
	CHECK_EQ(error, EPROTO);
	build_standard_lists(lists, "mlkem768x25519-sha256,kex-strict-s-v00@openssh.com");
	set_lists(&peer, lists, 0);
	error = ssh_kex_choose(1, 1, &peer, &choice, NULL);
	CHECK_EQ(error, EPROTO);

	/* A marker is a whole name: "kex-strict-c-v00" without the domain is not one. */
	build_standard_lists(lists, "mlkem768x25519-sha256,kex-strict-c-v00");
	set_lists(&peer, lists, 0);
	error = ssh_kex_choose(1, 1, &peer, &choice, NULL);
	CHECK_EQ(error, EPROTO);

	/* In a later exchange the markers are not looked at, and strict stays as it was. */
	build_standard_lists(lists, "mlkem768x25519-sha256");
	set_lists(&peer, lists, 0);
	choice.strict = 1;
	choice.ignore_next = 1;
	error = ssh_kex_choose(1, 0, &peer, &choice, NULL);
	CHECK_EQ(error, 0);
	CHECK_EQ(choice.strict, 1);
	CHECK_EQ(choice.ignore_next, 0);
	choice.strict = 0;
	error = ssh_kex_choose(0, 0, &peer, &choice, NULL);
	CHECK_EQ(error, 0);
	CHECK_EQ(choice.strict, 0);

	/* The ENOTSUP of a missing algorithm comes before the strict check, and leaves the choice alone. */
	build_standard_lists(lists, "curve25519-sha256");
	set_lists(&peer, lists, 0);
	choice.strict = 1;
	choice.ignore_next = 1;
	error = ssh_kex_choose(1, 1, &peer, &choice, NULL);
	CHECK_EQ(error, ENOTSUP);
	CHECK_EQ(choice.strict, 1);
	CHECK_EQ(choice.ignore_next, 1);
}

/* Checks first_kex_packet_follows: a right guess is used, a wrong guess is ignored. */
static void
test_choose_guess(void)
{
	struct ssh_kexinit peer;
	struct ssh_kex_choice choice;
	const char *lists[TEST_LISTS];
	int error;

	/* No guessed packet follows: nothing to ignore, whatever the order of the lists. */
	build_standard_lists(lists, "curve25519-sha256,mlkem768x25519-sha256,kex-strict-c");
	set_lists(&peer, lists, 0);
	memset(&choice, 0, sizeof(choice));
	error = ssh_kex_choose(1, 1, &peer, &choice, NULL);
	CHECK_EQ(error, 0);
	CHECK_EQ(choice.ignore_next, 0);

	/* A guess with our method and our host key first is right. */
	build_standard_lists(lists, "mlkem768x25519-sha256,curve25519-sha256,kex-strict-c");
	lists[1] = "ssh-ed25519,rsa-sha2-512";
	set_lists(&peer, lists, 1);
	error = ssh_kex_choose(1, 1, &peer, &choice, NULL);
	CHECK_EQ(error, 0);
	CHECK_EQ(choice.ignore_next, 0);

	/* A guess whose key exchange method is not first is wrong. */
	build_standard_lists(lists, "curve25519-sha256,mlkem768x25519-sha256,kex-strict-c");
	set_lists(&peer, lists, 1);
	error = ssh_kex_choose(1, 1, &peer, &choice, NULL);
	CHECK_EQ(error, 0);
	CHECK_EQ(choice.ignore_next, 1);

	/* A guess whose host key algorithm is not first is wrong. */
	build_standard_lists(lists, "mlkem768x25519-sha256,kex-strict-c");
	lists[1] = "rsa-sha2-512,ssh-ed25519";
	set_lists(&peer, lists, 1);
	error = ssh_kex_choose(1, 1, &peer, &choice, NULL);
	CHECK_EQ(error, 0);
	CHECK_EQ(choice.ignore_next, 1);

	/* A first name that only begins like ours is another name. */
	build_standard_lists(lists, "mlkem768x25519-sha256-x,mlkem768x25519-sha256,kex-strict-c");
	set_lists(&peer, lists, 1);
	error = ssh_kex_choose(1, 1, &peer, &choice, NULL);
	CHECK_EQ(error, 0);
	CHECK_EQ(choice.ignore_next, 1);
	build_standard_lists(lists, "mlkem768x25519-sha256,kex-strict-c");
	lists[1] = "ssh-ed25519-cert-v01@openssh.com,ssh-ed25519";
	set_lists(&peer, lists, 1);
	error = ssh_kex_choose(1, 1, &peer, &choice, NULL);
	CHECK_EQ(error, 0);
	CHECK_EQ(choice.ignore_next, 1);

	/* A guess in a later exchange is judged the same way. */
	build_standard_lists(lists, "curve25519-sha256,mlkem768x25519-sha256");
	set_lists(&peer, lists, 1);
	error = ssh_kex_choose(0, 0, &peer, &choice, NULL);
	CHECK_EQ(error, 0);
	CHECK_EQ(choice.ignore_next, 1);
}

/* Parses an OpenSSH-style client KEXINIT and chooses from it, with its flag clear and set. */
static void
test_choose_openssh(void)
{
	static uint8_t kept[WIRE_MAX];
	struct ssh_kexinit peer;
	struct ssh_kex_choice choice;
	const char *lists[TEST_LISTS];
	int error;

	/* The client's lists, from the defaults of OpenSSH 10.5; the server (this side) must accept. */
	lists[0] = openssh_kex;
	lists[1] = openssh_hostkey;
	lists[2] = openssh_cipher;
	lists[3] = openssh_cipher;
	lists[4] = openssh_mac;
	lists[5] = openssh_mac;
	lists[6] = "none,zlib@openssh.com";
	lists[7] = "none,zlib@openssh.com";
	lists[8] = "";
	lists[9] = "";
	build_kexinit(&payload_wire, lists, 0, 0);
	memcpy(kept, payload_wire.data, payload_wire.size);
	parse_ok(kept, payload_wire.size, &peer, 0);
	memset(&choice, 0, sizeof(choice));
	error = ssh_kex_choose(1, 1, &peer, &choice, NULL);
	CHECK_EQ(error, 0);
	CHECK_EQ(choice.strict, 1);
	CHECK_EQ(choice.ignore_next, 0);

	/* With the flag set the guess is right on the method but wrong on the host key (ssh-ed25519 is not first). */
	build_kexinit(&payload_wire, lists, 1, 0);
	memcpy(kept, payload_wire.data, payload_wire.size);
	parse_ok(kept, payload_wire.size, &peer, 0);
	error = ssh_kex_choose(1, 1, &peer, &choice, NULL);
	CHECK_EQ(error, 0);
	CHECK_EQ(choice.ignore_next, 1);

	/* The same peer, if it were a server, has no server marker: this side as a client refuses it. */
	error = ssh_kex_choose(0, 1, &peer, &choice, NULL);
	CHECK_EQ(error, EPROTO);
}

/* Checks the hybrid init message against bytes built with wire.h and its errors. */
static void
test_hybrid_init(void)
{
	static uint8_t c_init[SSH_HYBRID_INIT_SIZE];
	struct ssh_buf out;
	const uint8_t *value;
	size_t value_size;
	size_t length;
	uint8_t *copy;
	int error;

	fill_pattern(c_init, sizeof(c_init), 29, 11);

	/* The message: number 30, then a string. */
	ssh_buf_init(&out, 4096, 0);
	ssh_kex_put_hybrid_init(c_init, &out);
	CHECK_EQ(ssh_buf_error(&out), 0);
	wire_reset(&payload_wire);
	wire_u8(&payload_wire, 30);
	wire_string(&payload_wire, c_init, sizeof(c_init));
	CHECK_EQ(out.size, payload_wire.size);
	if (out.size == payload_wire.size)
		CHECK_BYTES(out.data, payload_wire.data, out.size);
	CHECK_EQ(out.size, 1U + 4U + SSH_HYBRID_INIT_SIZE);

	/* The parse finds the string inside the payload. */
	error = ssh_kex_parse_hybrid_init(payload_wire.data, payload_wire.size, &value, &value_size);
	CHECK_EQ(error, 0);
	CHECK_EQ(value_size, SSH_HYBRID_INIT_SIZE);
	CHECK(value == payload_wire.data + 5);
	CHECK_BYTES(value, c_init, SSH_HYBRID_INIT_SIZE);

	/* The size is not judged here: a string of another size is returned as it is. */
	wire_reset(&second_wire);
	wire_u8(&second_wire, 30);
	wire_string(&second_wire, c_init, 17);
	error = ssh_kex_parse_hybrid_init(second_wire.data, second_wire.size, &value, &value_size);
	CHECK_EQ(error, 0);
	CHECK_EQ(value_size, 17);
	wire_reset(&second_wire);
	wire_u8(&second_wire, 30);
	wire_string(&second_wire, NULL, 0);
	error = ssh_kex_parse_hybrid_init(second_wire.data, second_wire.size, &value, &value_size);
	CHECK_EQ(error, 0);
	CHECK_EQ(value_size, 0);

	/* A wrong message number, a trailing byte, a length past the end. */
	wire_reset(&second_wire);
	wire_u8(&second_wire, 31);
	wire_string(&second_wire, c_init, sizeof(c_init));
	error = ssh_kex_parse_hybrid_init(second_wire.data, second_wire.size, &value, &value_size);
	CHECK_EQ(error, EINVAL);
	CHECK(value == NULL);
	CHECK_EQ(value_size, 0);
	wire_reset(&second_wire);
	wire_u8(&second_wire, 30);
	wire_string(&second_wire, c_init, sizeof(c_init));
	wire_u8(&second_wire, 0);
	error = ssh_kex_parse_hybrid_init(second_wire.data, second_wire.size, &value, &value_size);
	CHECK_EQ(error, EINVAL);
	wire_reset(&second_wire);
	wire_u8(&second_wire, 30);
	wire_u32(&second_wire, 0xffffffffU);
	wire_bytes(&second_wire, c_init, 40);
	error = ssh_kex_parse_hybrid_init(second_wire.data, second_wire.size, &value, &value_size);
	CHECK_EQ(error, EINVAL);

	/* Every proper prefix is refused (copied into memory of exactly its size). */
	for (length = 0; length < payload_wire.size; length++) {
		copy = malloc(length == 0 ? 1 : length);
		if (copy == NULL)
			abort();
		if (length > 0)
			memcpy(copy, payload_wire.data, length);
		error = ssh_kex_parse_hybrid_init(copy, length, &value, &value_size);
		CHECK_EQ(error, EINVAL);
		free(copy);
	}

	ssh_buf_free(&out);
}

/* Checks the hybrid reply message against bytes built with wire.h. */
static void
test_hybrid_reply(void)
{
	static uint8_t host_key[51];
	static uint8_t s_reply[SSH_HYBRID_REPLY_SIZE];
	static uint8_t signature[83];
	struct ssh_buf out;
	const uint8_t *key_value;
	size_t key_size;
	const uint8_t *reply_value;
	size_t reply_size;
	const uint8_t *signature_value;
	size_t signature_size;
	int error;

	fill_pattern(host_key, sizeof(host_key), 3, 1);
	fill_pattern(s_reply, sizeof(s_reply), 7, 2);
	fill_pattern(signature, sizeof(signature), 11, 3);

	/* The message: number 31, then K_S, S_REPLY, the signature, each a string. */
	ssh_buf_init(&out, 8192, 0);
	ssh_kex_put_hybrid_reply(host_key, sizeof(host_key), s_reply, signature, sizeof(signature), &out);
	CHECK_EQ(ssh_buf_error(&out), 0);
	wire_reset(&payload_wire);
	wire_u8(&payload_wire, 31);
	wire_string(&payload_wire, host_key, sizeof(host_key));
	wire_string(&payload_wire, s_reply, sizeof(s_reply));
	wire_string(&payload_wire, signature, sizeof(signature));
	CHECK_EQ(out.size, payload_wire.size);
	if (out.size == payload_wire.size)
		CHECK_BYTES(out.data, payload_wire.data, out.size);

	/* The parse finds the three strings, in order, inside the payload. */
	error = ssh_kex_parse_hybrid_reply(payload_wire.data, payload_wire.size, &key_value, &key_size,
	    &reply_value, &reply_size, &signature_value, &signature_size);
	CHECK_EQ(error, 0);
	CHECK_EQ(key_size, sizeof(host_key));
	CHECK_EQ(reply_size, SSH_HYBRID_REPLY_SIZE);
	CHECK_EQ(signature_size, sizeof(signature));
	CHECK(key_value == payload_wire.data + 5);
	CHECK(reply_value == payload_wire.data + 5 + sizeof(host_key) + 4);
	CHECK(signature_value == reply_value + SSH_HYBRID_REPLY_SIZE + 4);
	CHECK_BYTES(key_value, host_key, sizeof(host_key));
	CHECK_BYTES(reply_value, s_reply, SSH_HYBRID_REPLY_SIZE);
	CHECK_BYTES(signature_value, signature, sizeof(signature));

	/* Empty strings are legal to this layer (their meaning is judged by the caller). */
	ssh_buf_reset(&out);
	ssh_kex_put_hybrid_reply(NULL, 0, s_reply, NULL, 0, &out);
	CHECK_EQ(ssh_buf_error(&out), 0);
	error = ssh_kex_parse_hybrid_reply(out.data, out.size, &key_value, &key_size,
	    &reply_value, &reply_size, &signature_value, &signature_size);
	CHECK_EQ(error, 0);
	CHECK_EQ(key_size, 0);
	CHECK_EQ(signature_size, 0);
	CHECK_EQ(reply_size, SSH_HYBRID_REPLY_SIZE);

	/* A buffer that is too small records the error. */
	ssh_buf_free(&out);
	ssh_buf_init(&out, 100, 0);
	ssh_kex_put_hybrid_reply(host_key, sizeof(host_key), s_reply, signature, sizeof(signature), &out);
	CHECK_EQ(ssh_buf_error(&out), EMSGSIZE);
	ssh_buf_free(&out);
}

/* Checks the malformed hybrid replies. */
static void
test_hybrid_errors(void)
{
	static uint8_t bytes[64];
	const uint8_t *key_value;
	size_t key_size;
	const uint8_t *reply_value;
	size_t reply_size;
	const uint8_t *signature_value;
	size_t signature_size;
	size_t length;
	uint8_t *copy;
	int error;

	fill_pattern(bytes, sizeof(bytes), 5, 5);

	/* A good message to cut and spoil. */
	wire_reset(&payload_wire);
	wire_u8(&payload_wire, 31);
	wire_string(&payload_wire, bytes, 20);
	wire_string(&payload_wire, bytes, 30);
	wire_string(&payload_wire, bytes, 10);
	error = ssh_kex_parse_hybrid_reply(payload_wire.data, payload_wire.size, &key_value, &key_size,
	    &reply_value, &reply_size, &signature_value, &signature_size);
	CHECK_EQ(error, 0);

	/* Every proper prefix is refused, with the outputs cleared. */
	for (length = 0; length < payload_wire.size; length++) {
		copy = malloc(length == 0 ? 1 : length);
		if (copy == NULL)
			abort();
		if (length > 0)
			memcpy(copy, payload_wire.data, length);
		error = ssh_kex_parse_hybrid_reply(copy, length, &key_value, &key_size,
		    &reply_value, &reply_size, &signature_value, &signature_size);
		CHECK_EQ(error, EINVAL);
		CHECK(key_value == NULL);
		CHECK(reply_value == NULL);
		CHECK(signature_value == NULL);
		CHECK_EQ(key_size + reply_size + signature_size, 0);
		free(copy);
	}

	/* A trailing byte. */
	wire_u8(&payload_wire, 0);
	error = ssh_kex_parse_hybrid_reply(payload_wire.data, payload_wire.size, &key_value, &key_size,
	    &reply_value, &reply_size, &signature_value, &signature_size);
	CHECK_EQ(error, EINVAL);

	/* A wrong message number (the init's, and zero). */
	payload_wire.size--;
	payload_wire.data[0] = 30;
	error = ssh_kex_parse_hybrid_reply(payload_wire.data, payload_wire.size, &key_value, &key_size,
	    &reply_value, &reply_size, &signature_value, &signature_size);
	CHECK_EQ(error, EINVAL);
	payload_wire.data[0] = 0;
	error = ssh_kex_parse_hybrid_reply(payload_wire.data, payload_wire.size, &key_value, &key_size,
	    &reply_value, &reply_size, &signature_value, &signature_size);
	CHECK_EQ(error, EINVAL);

	/* A string length of 2^32 - 1 and one that is only one byte too long. */
	wire_reset(&payload_wire);
	wire_u8(&payload_wire, 31);
	wire_u32(&payload_wire, 0xffffffffU);
	wire_bytes(&payload_wire, bytes, 40);
	error = ssh_kex_parse_hybrid_reply(payload_wire.data, payload_wire.size, &key_value, &key_size,
	    &reply_value, &reply_size, &signature_value, &signature_size);
	CHECK_EQ(error, EINVAL);
	wire_reset(&payload_wire);
	wire_u8(&payload_wire, 31);
	wire_string(&payload_wire, bytes, 20);
	wire_string(&payload_wire, bytes, 30);
	wire_u32(&payload_wire, 11);
	wire_bytes(&payload_wire, bytes, 10);
	error = ssh_kex_parse_hybrid_reply(payload_wire.data, payload_wire.size, &key_value, &key_size,
	    &reply_value, &reply_size, &signature_value, &signature_size);
	CHECK_EQ(error, EINVAL);
}

/* Checks the exchange hash against the known answer and against a hash built here from wire.h. */
static void
test_hash(void)
{
	struct ssh_kex_hash_input input;
	uint8_t h[SSH_KEX_HASH_SIZE];
	uint8_t expected[SSH_KEX_HASH_SIZE];
	int error;

	make_input(&input);

	/* The known answer. */
	memset(h, 0xee, sizeof(h));
	error = ssh_kex_hash(&input, h);
	CHECK_EQ(error, 0);
	expect_hex(h, kat_hash, SSH_KEX_HASH_SIZE, "exchange hash known answer");

	/* The hash of the eight strings built here: V_C, V_S, I_C, I_S, K_S, C_INIT, S_REPLY, K (a string of 32 bytes). */
	wire_reset(&payload_wire);
	wire_text(&payload_wire, in_version_client);
	wire_text(&payload_wire, in_version_server);
	wire_string(&payload_wire, in_kexinit_client, sizeof(in_kexinit_client));
	wire_string(&payload_wire, in_kexinit_server, sizeof(in_kexinit_server));
	wire_string(&payload_wire, in_host_key, sizeof(in_host_key));
	wire_string(&payload_wire, in_c_init, sizeof(in_c_init));
	wire_string(&payload_wire, in_s_reply, sizeof(in_s_reply));
	wire_string(&payload_wire, in_k, sizeof(in_k));
	CHECK_EQ(payload_wire.overflow, 0);
	error = ssh_sha256(payload_wire.data, payload_wire.size, expected);
	CHECK_EQ(error, 0);
	CHECK_BYTES(h, expected, SSH_KEX_HASH_SIZE);

	/* K as an mpint would give another hash (the first bit of the pattern's first byte differs from the 32-byte string). */
	wire_reset(&second_wire);
	wire_text(&second_wire, in_version_client);
	wire_text(&second_wire, in_version_server);
	wire_string(&second_wire, in_kexinit_client, sizeof(in_kexinit_client));
	wire_string(&second_wire, in_kexinit_server, sizeof(in_kexinit_server));
	wire_string(&second_wire, in_host_key, sizeof(in_host_key));
	wire_string(&second_wire, in_c_init, sizeof(in_c_init));
	wire_string(&second_wire, in_s_reply, sizeof(in_s_reply));
	wire_u32(&second_wire, 31);
	wire_bytes(&second_wire, in_k + 1, 31);
	error = ssh_sha256(second_wire.data, second_wire.size, expected);
	CHECK_EQ(error, 0);
	CHECK(memcmp(h, expected, SSH_KEX_HASH_SIZE) != 0);

	/* Empty fields are allowed (NULL with size 0). */
	input.host_key = NULL;
	input.host_key_size = 0;
	input.kexinit_client = NULL;
	input.kexinit_client_size = 0;
	error = ssh_kex_hash(&input, h);
	CHECK_EQ(error, 0);
	wire_reset(&payload_wire);
	wire_text(&payload_wire, in_version_client);
	wire_text(&payload_wire, in_version_server);
	wire_string(&payload_wire, NULL, 0);
	wire_string(&payload_wire, in_kexinit_server, sizeof(in_kexinit_server));
	wire_string(&payload_wire, NULL, 0);
	wire_string(&payload_wire, in_c_init, sizeof(in_c_init));
	wire_string(&payload_wire, in_s_reply, sizeof(in_s_reply));
	wire_string(&payload_wire, in_k, sizeof(in_k));
	error = ssh_sha256(payload_wire.data, payload_wire.size, expected);
	CHECK_EQ(error, 0);
	CHECK_BYTES(h, expected, SSH_KEX_HASH_SIZE);
}

/* Changes each field of the hash input in turn: H changes each time, and the order of the fields matters. */
static void
test_hash_sensitivity(void)
{
	struct ssh_kex_hash_input input;
	struct ssh_kex_hash_input spoiled;
	uint8_t base[SSH_KEX_HASH_SIZE];
	uint8_t h[SSH_KEX_HASH_SIZE];
	static uint8_t copy_client[37];
	static uint8_t copy_k[32];
	static const uint8_t joined[] = "SSH-2.0-zedBSD_1.0SSH-2.0-OpenSSH_10.5";
	unsigned which;
	int error;

	make_input(&input);

	/* A boundary moved between the two identification lines (the same joined bytes, split one byte earlier) gives another hash. */
	input.version_client = joined;
	input.version_client_size = 18;
	input.version_server = joined + 18;
	input.version_server_size = 20;
	error = ssh_kex_hash(&input, base);
	CHECK_EQ(error, 0);
	spoiled = input;
	spoiled.version_client_size = 17;
	spoiled.version_server = joined + 17;
	spoiled.version_server_size = 21;
	error = ssh_kex_hash(&spoiled, h);
	CHECK_EQ(error, 0);
	CHECK(memcmp(h, base, SSH_KEX_HASH_SIZE) != 0);

	/* The two identification lines swapped. */
	spoiled = input;
	spoiled.version_client = input.version_server;
	spoiled.version_client_size = input.version_server_size;
	spoiled.version_server = input.version_client;
	spoiled.version_server_size = input.version_client_size;
	error = ssh_kex_hash(&spoiled, h);
	CHECK_EQ(error, 0);
	CHECK(memcmp(h, base, SSH_KEX_HASH_SIZE) != 0);

	/* The two KEXINIT payloads swapped (their sizes differ). */
	spoiled = input;
	spoiled.kexinit_client = input.kexinit_server;
	spoiled.kexinit_client_size = input.kexinit_server_size;
	spoiled.kexinit_server = input.kexinit_client;
	spoiled.kexinit_server_size = input.kexinit_client_size;
	error = ssh_kex_hash(&spoiled, h);
	CHECK_EQ(error, 0);
	CHECK(memcmp(h, base, SSH_KEX_HASH_SIZE) != 0);

	/* One byte of each field changed; the field's contents in the input are patterned bytes. */
	for (which = 0; which < 6; which++) {
		spoiled = input;
		switch (which) {
		case 0:
			memcpy(copy_client, in_kexinit_client, sizeof(copy_client));
			copy_client[36] ^= 1;
			spoiled.kexinit_client = copy_client;
			break;
		case 1:
			in_kexinit_server[40] ^= 1;
			break;
		case 2:
			in_host_key[0] ^= 0x80;
			break;
		case 3:
			in_c_init[SSH_HYBRID_INIT_SIZE - 1] ^= 1;
			break;
		case 4:
			in_s_reply[SSH_HYBRID_REPLY_SIZE - 1] ^= 1;
			break;
		default:
			memcpy(copy_k, in_k, sizeof(copy_k));
			copy_k[31] ^= 1;
			spoiled.k = copy_k;
			break;
		}

		error = ssh_kex_hash(&spoiled, h);
		CHECK_EQ(error, 0);
		CHECK(memcmp(h, base, SSH_KEX_HASH_SIZE) != 0);

		/* Puts the changed byte back. */
		switch (which) {
		case 1:
			in_kexinit_server[40] ^= 1;
			break;
		case 2:
			in_host_key[0] ^= 0x80;
			break;
		case 3:
			in_c_init[SSH_HYBRID_INIT_SIZE - 1] ^= 1;
			break;
		case 4:
			in_s_reply[SSH_HYBRID_REPLY_SIZE - 1] ^= 1;
			break;
		default:
			break;
		}
	}

	/* After putting everything back, H is the base again. */
	error = ssh_kex_hash(&input, h);
	CHECK_EQ(error, 0);
	CHECK_BYTES(h, base, SSH_KEX_HASH_SIZE);
}

/* Checks the key derivation against known answers and against blocks built here from wire.h. */
static void
test_derive(void)
{
	uint8_t k[32];
	uint8_t h[32];
	uint8_t session_id[32];
	uint8_t out[256];
	uint8_t full[256];
	uint8_t expected[256];
	uint8_t block[32];
	uint8_t others[6][32];
	uint8_t letter_byte;
	size_t size;
	size_t made;
	unsigned a;
	unsigned b;
	int error;

	fill_pattern(k, sizeof(k), 17, 6);
	fill_pattern(h, sizeof(h), 23, 9);
	fill_pattern(session_id, sizeof(session_id), 19, 7);

	/* The known answers: C and D of 64 bytes (the ChaCha-Poly keys), A of 256, F of 33. */
	error = ssh_kex_derive(k, h, session_id, 'C', out, 64);
	CHECK_EQ(error, 0);
	expect_hex(out, kat_derive_c, 64, "derive C known answer");
	error = ssh_kex_derive(k, h, session_id, 'D', out, 64);
	CHECK_EQ(error, 0);
	expect_hex(out, kat_derive_d, 64, "derive D known answer");
	error = ssh_kex_derive(k, h, session_id, 'A', full, 256);
	CHECK_EQ(error, 0);
	expect_hex(full, kat_derive_a, 256, "derive A known answer");
	error = ssh_kex_derive(k, h, session_id, 'F', out, 33);
	CHECK_EQ(error, 0);
	expect_hex(out, kat_derive_f33, 33, "derive F known answer");

	/* K1 = HASH(string K || H || 'C' || session_id), built here from wire.h. */
	wire_reset(&payload_wire);
	wire_string(&payload_wire, k, sizeof(k));
	wire_bytes(&payload_wire, h, sizeof(h));
	wire_u8(&payload_wire, 'C');
	wire_bytes(&payload_wire, session_id, sizeof(session_id));
	error = ssh_sha256(payload_wire.data, payload_wire.size, expected);
	CHECK_EQ(error, 0);
	error = ssh_kex_derive(k, h, session_id, 'C', out, 32);
	CHECK_EQ(error, 0);
	CHECK_BYTES(out, expected, 32);

	/* K2 = HASH(string K || H || K1), the second block of the 64 bytes. */
	wire_reset(&payload_wire);
	wire_string(&payload_wire, k, sizeof(k));
	wire_bytes(&payload_wire, h, sizeof(h));
	wire_bytes(&payload_wire, expected, 32);
	error = ssh_sha256(payload_wire.data, payload_wire.size, block);
	CHECK_EQ(error, 0);
	error = ssh_kex_derive(k, h, session_id, 'C', out, 64);
	CHECK_EQ(error, 0);
	CHECK_BYTES(out, expected, 32);
	CHECK_BYTES(out + 32, block, 32);

	/* Every size from 1 to 256 is a prefix of the 256 bytes (also when the last block is partial). */
	for (size = 1; size <= 256; size++) {
		memset(out, 0xee, sizeof(out));
		error = ssh_kex_derive(k, h, session_id, 'A', out, size);
		CHECK_EQ(error, 0);
		CHECK_BYTES(out, full, size);
		if (size < 256)
			CHECK_EQ(out[size], 0xee);
	}

	/* The whole 256 bytes chain eight blocks: block n hashes K, H and all earlier blocks. */
	made = 0;
	for (size = 0; size < 8; size++) {
		wire_reset(&payload_wire);
		wire_string(&payload_wire, k, sizeof(k));
		wire_bytes(&payload_wire, h, sizeof(h));
		if (size == 0) {
			letter_byte = 'A';
			wire_u8(&payload_wire, letter_byte);
			wire_bytes(&payload_wire, session_id, sizeof(session_id));
		} else {
			wire_bytes(&payload_wire, expected, made);
		}

		error = ssh_sha256(payload_wire.data, payload_wire.size, block);
		CHECK_EQ(error, 0);
		memcpy(expected + made, block, 32);
		made += 32;
	}

	CHECK_BYTES(full, expected, 256);

	/* A different letter, session identifier, H or K gives different material (the six letters differ pairwise). */
	for (a = 0; a < 6; a++) {
		error = ssh_kex_derive(k, h, session_id, (char)('A' + a), others[a], 32);
		CHECK_EQ(error, 0);
	}

	for (a = 0; a < 6; a++) {
		for (b = a + 1; b < 6; b++)
			CHECK(memcmp(others[a], others[b], 32) != 0);
	}

	session_id[0] ^= 1;
	error = ssh_kex_derive(k, h, session_id, 'A', out, 32);
	CHECK_EQ(error, 0);
	CHECK(memcmp(out, full, 32) != 0);
	session_id[0] ^= 1;
	h[31] ^= 1;
	error = ssh_kex_derive(k, h, session_id, 'A', out, 32);
	CHECK_EQ(error, 0);
	CHECK(memcmp(out, full, 32) != 0);
	h[31] ^= 1;
	k[0] ^= 1;
	error = ssh_kex_derive(k, h, session_id, 'A', out, 32);
	CHECK_EQ(error, 0);
	CHECK(memcmp(out, full, 32) != 0);
	k[0] ^= 1;
}

/* Checks the arguments ssh_kex_derive refuses. */
static void
test_derive_errors(void)
{
	uint8_t k[32];
	uint8_t h[32];
	uint8_t session_id[32];
	uint8_t out[300];
	int error;

	fill_pattern(k, sizeof(k), 17, 6);
	fill_pattern(h, sizeof(h), 23, 9);
	fill_pattern(session_id, sizeof(session_id), 19, 7);

	/* Letters outside 'A' to 'F': the neighbours of the range, a lower case letter, NUL, a high value. */
	error = ssh_kex_derive(k, h, session_id, '@', out, 32);
	CHECK_EQ(error, EINVAL);
	error = ssh_kex_derive(k, h, session_id, 'G', out, 32);
	CHECK_EQ(error, EINVAL);
	error = ssh_kex_derive(k, h, session_id, 'a', out, 32);
	CHECK_EQ(error, EINVAL);
	error = ssh_kex_derive(k, h, session_id, '\0', out, 32);
	CHECK_EQ(error, EINVAL);
	error = ssh_kex_derive(k, h, session_id, (char)0x7f, out, 32);
	CHECK_EQ(error, EINVAL);

	/* The edges of the letters are accepted. */
	error = ssh_kex_derive(k, h, session_id, 'A', out, 32);
	CHECK_EQ(error, 0);
	error = ssh_kex_derive(k, h, session_id, 'F', out, 32);
	CHECK_EQ(error, 0);

	/* Sizes 0 and 257 and above are refused; 1 and 256 are accepted. */
	error = ssh_kex_derive(k, h, session_id, 'C', out, 0);
	CHECK_EQ(error, EINVAL);
	error = ssh_kex_derive(k, h, session_id, 'C', out, 257);
	CHECK_EQ(error, EINVAL);
	error = ssh_kex_derive(k, h, session_id, 'C', out, 300);
	CHECK_EQ(error, EINVAL);
	error = ssh_kex_derive(k, h, session_id, 'C', out, (size_t)-1);
	CHECK_EQ(error, EINVAL);
	error = ssh_kex_derive(k, h, session_id, 'C', out, 1);
	CHECK_EQ(error, 0);
	error = ssh_kex_derive(k, h, session_id, 'C', out, 256);
	CHECK_EQ(error, 0);
}

/* Feeds random and damaged inputs to every parser and to the choice: none crashes (ASan, UBSan). */
static void
test_random_feed(void)
{
	static uint8_t valid[WIRE_MAX];
	struct ssh_kexinit kexinit;
	struct ssh_kex_choice choice;
	const uint8_t *a;
	size_t a_size;
	const uint8_t *b;
	size_t b_size;
	const uint8_t *c;
	size_t c_size;
	const char *lists[TEST_LISTS];
	uint8_t *copy;
	size_t size;
	size_t valid_size;
	size_t index;
	unsigned run;
	unsigned flips;
	unsigned success;
	int error;

	/* A valid KEXINIT to damage. */
	build_standard_lists(lists, "mlkem768x25519-sha256,kex-strict-c,kex-strict-c-v00@openssh.com");
	build_kexinit(&payload_wire, lists, 1, 0);
	valid_size = payload_wire.size;
	memcpy(valid, payload_wire.data, valid_size);

	success = 0;
	for (run = 0; run < TEST_RANDOM_RUNS; run++) {
		/* Takes a copy of the valid payload of exactly its size and flips a few bytes. */
		copy = malloc(valid_size);
		if (copy == NULL)
			abort();
		memcpy(copy, valid, valid_size);
		flips = 1U + next_random() % 4U;
		while (flips-- > 0U) {
			index = next_random() % valid_size;
			copy[index] = (uint8_t)next_random();
		}

		/* The parse never fails to end; the choice on a parsed result never fails to end either. */
		error = ssh_kexinit_parse(copy, valid_size, &kexinit);
		if (error == 0) {
			success++;
			memset(&choice, 0, sizeof(choice));
			error = ssh_kex_choose((int)(next_random() & 1U), (int)(next_random() & 1U), &kexinit, &choice, NULL);
			CHECK(error == 0 || error == ENOTSUP || error == EPROTO);
		} else {
			CHECK(error == EINVAL || error == EMSGSIZE);
		}

		free(copy);
	}

	/* Some damaged payloads still parse (a changed cookie byte), so the choice was exercised. */
	CHECK(success > 0);

	/* Random bytes of random length go to all three parsers, in memory of exactly their size. */
	for (run = 0; run < TEST_RANDOM_RUNS; run++) {
		size = next_random() % 200U;
		copy = malloc(size == 0 ? 1 : size);
		if (copy == NULL)
			abort();
		for (index = 0; index < size; index++)
			copy[index] = (uint8_t)next_random();
		if (size > 0 && (next_random() & 1U) != 0U)
			copy[0] = (uint8_t)(20U + (next_random() % 12U));
		error = ssh_kexinit_parse(copy, size, &kexinit);
		CHECK(error == 0 || error == EINVAL || error == EMSGSIZE);
		error = ssh_kex_parse_hybrid_init(copy, size, &a, &a_size);
		CHECK(error == 0 || error == EINVAL);
		error = ssh_kex_parse_hybrid_reply(copy, size, &a, &a_size, &b, &b_size, &c, &c_size);
		CHECK(error == 0 || error == EINVAL);
		free(copy);
	}
}
