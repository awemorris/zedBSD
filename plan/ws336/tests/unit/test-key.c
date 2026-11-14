/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The test of ssh-key.c.  The keys are the secret keys, public keys and
 * signatures of RFC 8032 section 7.1 (TEST 1 and TEST 2).  Every input is
 * built here from the layouts of the specifications with wire.h and an
 * encoder of base64 written apart from the module: the public key blob and
 * the signature blob of RFC 8709 sections 4 and 6, the one-line form of
 * sshd(8) AUTHORIZED_KEYS FILE FORMAT, and the private key file of OpenSSH
 * PROTOCOL.key sections 1, 3 and 5 with the Ed25519 agent encoding of
 * draft-ietf-sshm-ssh-agent-16 section 3.2.3.  The expected fingerprint is
 * the SHA-256 of the blob in base64 without padding (ssh-keygen(1) -l).
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "wire.h"
#include "ssh-key.h"

/* The size of the text buffers of this test, in bytes. */
#define TEST_TEXT_MAX		20000U

/* The number of random changes tried on a private key file. */
#define TEST_FUZZ_COUNT		100000UL

/*
 * The test keys, as hexadecimal text.
 *
 * Values referenced: RFC 8032 section 7.1, TEST 1 (secret key, public key,
 * signature of the empty message) and TEST 2 (secret key, public key,
 * message 72, signature).
 */
static const char t1_seed_hex[] =
	"9d61b19deffd5a60ba844af492ec2cc4 4449c5697b326919703bac031cae7f60";
static const char t1_public_hex[] =
	"d75a980182b10ab7d54bfed3c964073a 0ee172f3daa62325af021a68f707511a";
static const char t1_signature_hex[] =
	"e5564300c360ac729086e2cc806e828a 84877f1eb8e5d974d873e06522490155"
	"5fb8821590a33bacc61e39701cf9b46b d25bf5f0595bbe24655141438e7a100b";
static const char t2_seed_hex[] =
	"4ccd089b28ff96da9db6c346ec114e0f 5b8a319f35aba624da8cf6ed4fb8a6fb";
static const char t2_public_hex[] =
	"3d4017c3e843895a92b70aa74d1b7ebc 9c982ccf2ec4968cc0cd55f12af4660c";
static const char t2_signature_hex[] =
	"92a009a9f0d4cab8720e820b5f642540 a2b27b5416503f8fb3762223ebdb69da"
	"085ac1e43e15996e458f3613d0f11d8c 387b2eaeb4302aeeb00d291612bb0c00";

/*
 * The base64 of the public key blob of TEST 1 and the SHA-256 fingerprint
 * text of that blob, worked out by hand apart from the module (the blob is
 * 00 00 00 0b "ssh-ed25519" 00 00 00 20 and the 32 key bytes).
 */
static const char t1_blob_base64[] =
	"AAAAC3NzaC1lZDI1NTE5AAAAINdamAGCsQq31Uv+08lkBzoO4XLz2qYjJa8CGmj3B1Ea";
static const char t1_fingerprint[] =
	"SHA256:bbXpuKG6zhzdmnxq256TlqzFBzRl2f6OOg722cYNbU8";

/* The key bytes read from the hexadecimal text above. */
static uint8_t t1_seed[SSH_ED25519_SEED];
static uint8_t t1_public[SSH_ED25519_PUBLIC];
static uint8_t t1_signature[SSH_ED25519_SIGNATURE];
static uint8_t t2_seed[SSH_ED25519_SEED];
static uint8_t t2_public[SSH_ED25519_PUBLIC];
static uint8_t t2_signature[SSH_ED25519_SIGNATURE];

/* The message of TEST 2 (one byte, 0x72). */
static const uint8_t t2_message[1] = { 0x72 };

/* The ways the padding of the private section of a built file is wrong. */
#define PAD_GOOD		0
#define PAD_LAST_WRONG		1
#define PAD_ZEROS		2
#define PAD_EXTRA_BLOCK		3
#define PAD_SHORT		4

/* The width of a base64 line that means "one line". */
#define WRAP_ONE_LINE		0U

/*
 * What a built private key file holds.  Every field has a good value from
 * spec_init; a test changes the one it wants to break.
 */
struct file_spec {
	char magic[15];
	uint32_t check_first;
	uint32_t check_second;
	const char *cipher;
	const char *kdf;
	size_t options_size;
	uint32_t count;
	const char *blob_type;
	const uint8_t *blob_key;
	size_t blob_key_size;
	const char *inner_type;
	const uint8_t *inner_public;
	size_t inner_public_size;
	const uint8_t *pair_seed;
	const uint8_t *pair_public;
	size_t pair_size;
	const char *comment;
	size_t comment_size;
	int padding;
	size_t outer_extra;
	size_t outer_cut;
	size_t wrap;
	int crlf;
	int end_newline;
	int omit_begin;
	int omit_end;
	int blank_line;
	const char *begin_line;
	const char *prefix;
	const char *suffix;
};

/* The working buffers of this test: large, so declared static. */
static struct wire inner_wire;
static struct wire outer_wire;
static char file_text[TEST_TEXT_MAX];
static char base64_text[TEST_TEXT_MAX];

/* A long comment for the private key file tests (filled when used); the test runs in one thread. */
static char edge_comment[400];

static void load_vectors(void);
static void spec_init(struct file_spec *spec);
static size_t build_decoded(const struct file_spec *spec);
static size_t build_text(const struct file_spec *spec);
static void test_base64(const uint8_t *in, size_t size, char *out, int pad);
static void make_key(struct ssh_key *key, const uint8_t *seed, const uint8_t *public_key, const char *comment);
static void build_blob(struct wire *wire, const char *name, const uint8_t *key, size_t key_size);
static void build_signature(struct wire *wire, const char *name, const uint8_t *signature, size_t size);
static int key_is_zero(const struct ssh_key *key);
static int parse_spec(const struct file_spec *spec, struct ssh_key *key);
static void test_clear_generate(void);
static void test_blob(void);
static void test_sign_verify(void);
static void test_public_line(void);
static void test_private_good(void);
static void test_private_bad(void);
static void test_private_framing(void);
static void test_private_put(void);
static void test_fingerprint(void);
static void test_fuzz(void);

/* Runs every check of the key module and prints the last line. */
int
main(void)
{
	int status;

	/* Reads the test keys. */
	load_vectors();

	/* Runs each group of checks. */
	test_clear_generate();
	test_blob();
	test_sign_verify();
	test_public_line();
	test_private_good();
	test_private_bad();
	test_private_framing();
	test_private_put();
	test_fingerprint();
	test_fuzz();

	/* Prints the verdict. */
	status = check_finish("key");
	return status;
}

/* Reads the hexadecimal key texts into bytes and checks that each has the size it should. */
static void
load_vectors(void)
{
	size_t count;

	/* TEST 1. */
	count = wire_hex(t1_seed_hex, t1_seed, sizeof(t1_seed));
	CHECK_EQ(count, SSH_ED25519_SEED);
	count = wire_hex(t1_public_hex, t1_public, sizeof(t1_public));
	CHECK_EQ(count, SSH_ED25519_PUBLIC);
	count = wire_hex(t1_signature_hex, t1_signature, sizeof(t1_signature));
	CHECK_EQ(count, SSH_ED25519_SIGNATURE);

	/* TEST 2. */
	count = wire_hex(t2_seed_hex, t2_seed, sizeof(t2_seed));
	CHECK_EQ(count, SSH_ED25519_SEED);
	count = wire_hex(t2_public_hex, t2_public, sizeof(t2_public));
	CHECK_EQ(count, SSH_ED25519_PUBLIC);
	count = wire_hex(t2_signature_hex, t2_signature, sizeof(t2_signature));
	CHECK_EQ(count, SSH_ED25519_SIGNATURE);
}

/*
 * Encodes size bytes as base64 (RFC 4648 4, Table 1) into out with a NUL,
 * written apart from the module.  pad is 1 for '=' padding.
 */
static void
test_base64(
	const uint8_t *in,
	size_t size,
	char *out,
	int pad)
{
	static const char alphabet[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t position;
	size_t length;
	uint32_t group;
	size_t rest;

	/* Starts at the first input byte with no output. */
	position = 0;
	length = 0;

	/* Takes whole groups of three bytes into four characters. */
	while (size - position >= 3U) {
		group = ((uint32_t)in[position] << 16) | ((uint32_t)in[position + 1U] << 8) | in[position + 2U];
		out[length++] = alphabet[(group >> 18) & 63U];
		out[length++] = alphabet[(group >> 12) & 63U];
		out[length++] = alphabet[(group >> 6) & 63U];
		out[length++] = alphabet[group & 63U];
		position += 3U;
	}

	/* Takes the last one or two bytes. */
	rest = size - position;
	if (rest == 1U) {
		group = (uint32_t)in[position] << 16;
		out[length++] = alphabet[(group >> 18) & 63U];
		out[length++] = alphabet[(group >> 12) & 63U];
		if (pad) {
			out[length++] = '=';
			out[length++] = '=';
		}
	} else if (rest == 2U) {
		group = ((uint32_t)in[position] << 16) | ((uint32_t)in[position + 1U] << 8);
		out[length++] = alphabet[(group >> 18) & 63U];
		out[length++] = alphabet[(group >> 12) & 63U];
		out[length++] = alphabet[(group >> 6) & 63U];
		if (pad)
			out[length++] = '=';
	}

	/* Ends the text. */
	out[length] = '\0';
}

/* Fills a private key from a seed, its public key and a comment, without the module. */
static void
make_key(
	struct ssh_key *key,
	const uint8_t *seed,
	const uint8_t *public_key,
	const char *comment)
{
	/* Starts from zero bytes. */
	memset(key, 0, sizeof(*key));

	/* Copies the pieces. */
	memcpy(key->seed, seed, SSH_ED25519_SEED);
	memcpy(key->public_key, public_key, SSH_ED25519_PUBLIC);
	strcpy(key->comment, comment);
	key->private = 1;
}

/* Builds a public key blob layout: string name, string of key_size bytes. */
static void
build_blob(
	struct wire *wire,
	const char *name,
	const uint8_t *key,
	size_t key_size)
{
	/* The two strings. */
	wire_reset(wire);
	wire_text(wire, name);
	wire_string(wire, key, key_size);
}

/* Builds a signature blob layout: string name, string of size bytes. */
static void
build_signature(
	struct wire *wire,
	const char *name,
	const uint8_t *signature,
	size_t size)
{
	/* The two strings. */
	wire_reset(wire);
	wire_text(wire, name);
	wire_string(wire, signature, size);
}

/* Reports whether every byte of key is zero. */
static int
key_is_zero(
	const struct ssh_key *key)
{
	const unsigned char *bytes;
	size_t index;

	/* Views the key as bytes. */
	bytes = (const unsigned char *)key;

	/* Looks at each byte. */
	for (index = 0; index < sizeof(*key); index++) {
		if (bytes[index] != 0)
			return 0;
	}

	/* All zero. */
	return 1;
}

/* Sets the good value of every field of a private key file. */
static void
spec_init(
	struct file_spec *spec)
{
	/* Starts from zero bytes. */
	memset(spec, 0, sizeof(*spec));

	/* The magic with its NUL (PROTOCOL.key 1). */
	memcpy(spec->magic, "openssh-key-v1", 15);

	/* Two equal check integers. */
	spec->check_first = 0x12345678U;
	spec->check_second = 0x12345678U;

	/* An unencrypted file of one key. */
	spec->cipher = "none";
	spec->kdf = "none";
	spec->options_size = 0;
	spec->count = 1;

	/* The TEST 1 key in the blob, in the private key and in the pair. */
	spec->blob_type = "ssh-ed25519";
	spec->blob_key = t1_public;
	spec->blob_key_size = SSH_ED25519_PUBLIC;
	spec->inner_type = "ssh-ed25519";
	spec->inner_public = t1_public;
	spec->inner_public_size = SSH_ED25519_PUBLIC;
	spec->pair_seed = t1_seed;
	spec->pair_public = t1_public;
	spec->pair_size = SSH_ED25519_SEED + SSH_ED25519_PUBLIC;
	spec->comment = "ab";
	spec->comment_size = 2;

	/* Good padding, no extra bytes, no cut. */
	spec->padding = PAD_GOOD;

	/* Lines of 70 characters ending with LF, the file ending with a line feed. */
	spec->wrap = SSH_KEY_PRIVATE_WRAP;
	spec->crlf = 0;
	spec->end_newline = 1;
	spec->begin_line = SSH_KEY_PRIVATE_BEGIN;
}

/*
 * Builds the decoded bytes of a private key file for spec into outer_wire
 * and returns their size.  The layouts are those of PROTOCOL.key 1, 3, 5
 * and the agent encoding of draft-ietf-sshm-ssh-agent-16 3.2.3.
 */
static size_t
build_decoded(
	const struct file_spec *spec)
{
	uint8_t pair[SSH_ED25519_SEED + SSH_ED25519_PUBLIC + 8];
	size_t pad_size;
	size_t index;
	size_t padding_byte;
	uint8_t options[16];

	/* The seed followed by the public key. */
	memset(pair, 0, sizeof(pair));
	memcpy(pair, spec->pair_seed, SSH_ED25519_SEED);
	memcpy(pair + SSH_ED25519_SEED, spec->pair_public, SSH_ED25519_PUBLIC);

	/* The private section. */
	wire_reset(&inner_wire);
	wire_u32(&inner_wire, spec->check_first);
	wire_u32(&inner_wire, spec->check_second);
	wire_text(&inner_wire, spec->inner_type);
	wire_string(&inner_wire, spec->inner_public, spec->inner_public_size);
	wire_string(&inner_wire, pair, spec->pair_size);
	wire_string(&inner_wire, spec->comment, spec->comment_size);

	/* The padding the format asks for: the bytes 1, 2, 3, ... up to a multiple of 8. */
	pad_size = (8U - inner_wire.size % 8U) % 8U;
	if (spec->padding == PAD_GOOD) {
		for (index = 1; index <= pad_size; index++)
			wire_u8(&inner_wire, (unsigned)index);
	} else if (spec->padding == PAD_LAST_WRONG) {
		for (index = 1; index <= pad_size; index++) {
			padding_byte = index;
			if (index == pad_size)
				padding_byte = index + 1U;
			wire_u8(&inner_wire, (unsigned)padding_byte);
		}
	} else if (spec->padding == PAD_ZEROS) {
		for (index = 1; index <= pad_size; index++)
			wire_u8(&inner_wire, 0);
	} else if (spec->padding == PAD_EXTRA_BLOCK) {
		for (index = 1; index <= pad_size + 8U; index++)
			wire_u8(&inner_wire, (unsigned)index);
	} else if (spec->padding == PAD_SHORT) {
		for (index = 1; index < pad_size; index++)
			wire_u8(&inner_wire, (unsigned)index);
	}

	/* The outer layout: magic, cipher, KDF, options, key count, public key blob, private section. */
	wire_reset(&outer_wire);
	wire_bytes(&outer_wire, spec->magic, sizeof(spec->magic));
	wire_text(&outer_wire, spec->cipher);
	wire_text(&outer_wire, spec->kdf);
	memset(options, 0x5a, sizeof(options));
	wire_string(&outer_wire, options, spec->options_size);
	wire_u32(&outer_wire, spec->count);
	wire_u32(&outer_wire, (uint32_t)(4U + strlen(spec->blob_type) + 4U + spec->blob_key_size));
	wire_text(&outer_wire, spec->blob_type);
	wire_string(&outer_wire, spec->blob_key, spec->blob_key_size);
	wire_string(&outer_wire, inner_wire.data, inner_wire.size);

	/* Bytes after the section, and a cut of the end. */
	for (index = 0; index < spec->outer_extra; index++)
		wire_u8(&outer_wire, 0xee);
	if (spec->outer_cut > 0 && spec->outer_cut <= outer_wire.size)
		outer_wire.size -= spec->outer_cut;

	/* Reports the size. */
	return outer_wire.size;
}

/*
 * Builds the text of a private key file for spec into file_text and
 * returns its size: the optional prefix, the BEGIN line, the base64 in
 * lines of spec->wrap characters, the END line and the optional suffix.
 */
static size_t
build_text(
	const struct file_spec *spec)
{
	size_t decoded_size;
	size_t length;
	size_t encoded_size;
	size_t position;
	size_t chunk;
	size_t lines;
	size_t width;
	const char *eol;

	/* Builds the decoded content and encodes it as base64. */
	decoded_size = build_decoded(spec);
	test_base64(outer_wire.data, decoded_size, base64_text, 1);
	encoded_size = strlen(base64_text);

	/* Picks the line ending of the file. */
	eol = "\n";
	if (spec->crlf)
		eol = "\r\n";

	/* Starts the text empty. */
	length = 0;

	/* Text before the BEGIN line. */
	if (spec->prefix != NULL) {
		strcpy(file_text + length, spec->prefix);
		length += strlen(spec->prefix);
	}

	/* The BEGIN line. */
	if (!spec->omit_begin) {
		strcpy(file_text + length, spec->begin_line);
		length += strlen(spec->begin_line);
		strcpy(file_text + length, eol);
		length += strlen(eol);
	}

	/* The base64 lines. */
	width = spec->wrap;
	if (spec->wrap == WRAP_ONE_LINE)
		width = encoded_size + 1U;
	position = 0;
	lines = 0;
	while (position < encoded_size) {
		chunk = encoded_size - position;
		if (chunk > width)
			chunk = width;
		memcpy(file_text + length, base64_text + position, chunk);
		length += chunk;
		strcpy(file_text + length, eol);
		length += strlen(eol);
		position += chunk;
		lines++;

		/* An empty line in the middle of the base64. */
		if (spec->blank_line && lines == 1U) {
			strcpy(file_text + length, eol);
			length += strlen(eol);
		}
	}

	/* The END line. */
	if (!spec->omit_end) {
		strcpy(file_text + length, SSH_KEY_PRIVATE_END);
		length += strlen(SSH_KEY_PRIVATE_END);
		if (spec->end_newline) {
			strcpy(file_text + length, eol);
			length += strlen(eol);
		}
	}

	/* Text after the END line. */
	if (spec->suffix != NULL) {
		strcpy(file_text + length, spec->suffix);
		length += strlen(spec->suffix);
	}

	/* Reports the size. */
	return length;
}

/* Builds the file for spec and parses it into key; returns the module's result. */
static int
parse_spec(
	const struct file_spec *spec,
	struct ssh_key *key)
{
	size_t length;
	int error;

	/* Builds, then reads. */
	length = build_text(spec);
	error = ssh_key_parse_private(file_text, length, key);

	/* Reports the module's answer. */
	return error;
}

/* Checks ssh_key_clear and ssh_key_generate. */
static void
test_clear_generate(void)
{
	struct ssh_key key;
	struct ssh_key other;
	struct ssh_buf out;
	static const uint8_t message[3] = { 1, 2, 3 };
	uint8_t derived[SSH_ED25519_PUBLIC];
	uint8_t zeros[SSH_ED25519_SEED];
	char long_comment[300];
	size_t length;
	size_t expected_length;
	int error;

	/* Clearing sets every byte, the seed included, to zero. */
	make_key(&key, t1_seed, t1_public, "to be cleared");
	ssh_key_clear(&key);
	CHECK(key_is_zero(&key));

	/* A generated key is private, its public key is the one of the seed, and the seed is not zero. */
	memset(&key, 0xff, sizeof(key));
	error = ssh_key_generate(&key, "hello");
	CHECK_EQ(error, 0);
	CHECK_EQ(key.private, 1);
	CHECK(strcmp(key.comment, "hello") == 0);
	error = ssh_ed25519_public(key.seed, derived);
	CHECK_EQ(error, 0);
	CHECK_BYTES(key.public_key, derived, SSH_ED25519_PUBLIC);
	memset(zeros, 0, sizeof(zeros));
	CHECK(memcmp(key.seed, zeros, sizeof(zeros)) != 0);

	/* Two generated keys differ. */
	error = ssh_key_generate(&other, NULL);
	CHECK_EQ(error, 0);
	CHECK(memcmp(key.seed, other.seed, SSH_ED25519_SEED) != 0);
	CHECK(memcmp(key.public_key, other.public_key, SSH_ED25519_PUBLIC) != 0);

	/* No comment is the empty comment. */
	CHECK_EQ(other.comment[0], 0);

	/* A comment too long is cut to what fits and ends with a NUL. */
	memset(long_comment, 'c', sizeof(long_comment) - 1U);
	long_comment[sizeof(long_comment) - 1U] = '\0';
	error = ssh_key_generate(&other, long_comment);
	CHECK_EQ(error, 0);
	CHECK_EQ(strlen(other.comment), SSH_KEY_COMMENT_MAX - 1U);
	CHECK_EQ(other.comment[SSH_KEY_COMMENT_MAX - 1U], 0);
	CHECK_EQ(other.comment[0], 'c');

	/* Comments around the limit: up to 255 bytes are kept whole, longer ones are cut to 255. */
	for (length = 254; length <= 257U; length++) {
		memset(long_comment, 'd', length);
		long_comment[length] = '\0';
		error = ssh_key_generate(&other, long_comment);
		CHECK_EQ(error, 0);
		expected_length = length;
		if (expected_length > 255U)
			expected_length = 255U;
		CHECK_EQ(strlen(other.comment), expected_length);
	}

	/* A generated key signs and verifies. */
	ssh_buf_init(&out, 256, 0);
	error = ssh_key_sign(&key, message, sizeof(message), &out);
	CHECK_EQ(error, 0);
	CHECK_EQ(out.size, SSH_KEY_SIGNATURE_BLOB_SIZE);
	error = ssh_key_verify(key.public_key, message, sizeof(message), out.data, out.size);
	CHECK_EQ(error, 0);
	ssh_buf_free(&out);

	/* Wipes both keys. */
	ssh_key_clear(&key);
	ssh_key_clear(&other);
}

/* Checks the public key blob: layout, round trip and every malformed form. */
static void
test_blob(void)
{
	struct ssh_buf out;
	struct wire *wire;
	uint8_t parsed[SSH_ED25519_PUBLIC];
	uint8_t sentinel[SSH_ED25519_PUBLIC];
	uint8_t big_key[SSH_ED25519_PUBLIC + 1];
	char name[80];
	size_t length;
	int error;

	/* Makes a key one byte too long and a buffer of a known byte. */
	wire = &outer_wire;
	memset(big_key, 0x11, sizeof(big_key));
	memset(sentinel, 0xa5, sizeof(sentinel));

	/* The blob is string "ssh-ed25519" and string of the 32 bytes: 51 bytes. */
	build_blob(wire, "ssh-ed25519", t1_public, SSH_ED25519_PUBLIC);
	CHECK_EQ(wire->size, SSH_KEY_BLOB_SIZE);
	ssh_buf_init(&out, 256, 0);
	ssh_key_put_public_blob(t1_public, &out);
	CHECK_EQ(ssh_buf_error(&out), 0);
	CHECK_EQ(out.size, SSH_KEY_BLOB_SIZE);
	CHECK_BYTES(out.data, wire->data, SSH_KEY_BLOB_SIZE);

	/* It appends to what is there. */
	ssh_key_put_public_blob(t2_public, &out);
	CHECK_EQ(out.size, 2U * SSH_KEY_BLOB_SIZE);
	CHECK_BYTES(out.data, wire->data, SSH_KEY_BLOB_SIZE);
	ssh_buf_free(&out);

	/* A buffer too small records the error. */
	ssh_buf_init(&out, 20, 0);
	ssh_key_put_public_blob(t1_public, &out);
	CHECK_EQ(ssh_buf_error(&out), EMSGSIZE);
	ssh_buf_free(&out);

	/* Parsing gives the key back. */
	memset(parsed, 0, sizeof(parsed));
	error = ssh_key_parse_public_blob(wire->data, wire->size, parsed);
	CHECK_EQ(error, 0);
	CHECK_BYTES(parsed, t1_public, SSH_ED25519_PUBLIC);

	/* Another algorithm is ENOTSUP, and the output is not written. */
	build_blob(wire, "ssh-rsa", t1_public, SSH_ED25519_PUBLIC);
	memset(parsed, 0xa5, sizeof(parsed));
	error = ssh_key_parse_public_blob(wire->data, wire->size, parsed);
	CHECK_EQ(error, ENOTSUP);
	CHECK_BYTES(parsed, sentinel, sizeof(parsed));

	/* The name alone decides ENOTSUP, whatever follows it. */
	wire_reset(wire);
	wire_text(wire, "ssh-rsa");
	wire_u32(wire, 1000);
	error = ssh_key_parse_public_blob(wire->data, wire->size, parsed);
	CHECK_EQ(error, ENOTSUP);

	/* Names that differ from ssh-ed25519 in one place or in case are other algorithms. */
	build_blob(wire, "ssh-ed25518", t1_public, SSH_ED25519_PUBLIC);
	CHECK_EQ(ssh_key_parse_public_blob(wire->data, wire->size, parsed), ENOTSUP);
	build_blob(wire, "ssh-ed2551", t1_public, SSH_ED25519_PUBLIC);
	CHECK_EQ(ssh_key_parse_public_blob(wire->data, wire->size, parsed), ENOTSUP);
	build_blob(wire, "ssh-ed255199", t1_public, SSH_ED25519_PUBLIC);
	CHECK_EQ(ssh_key_parse_public_blob(wire->data, wire->size, parsed), ENOTSUP);
	build_blob(wire, "SSH-ED25519", t1_public, SSH_ED25519_PUBLIC);
	CHECK_EQ(ssh_key_parse_public_blob(wire->data, wire->size, parsed), ENOTSUP);

	/* An empty name and a name over 64 bytes cannot be an algorithm name. */
	build_blob(wire, "", t1_public, SSH_ED25519_PUBLIC);
	CHECK_EQ(ssh_key_parse_public_blob(wire->data, wire->size, parsed), EINVAL);
	memset(name, 'a', 65);
	name[65] = '\0';
	build_blob(wire, name, t1_public, SSH_ED25519_PUBLIC);
	CHECK_EQ(ssh_key_parse_public_blob(wire->data, wire->size, parsed), EINVAL);

	/* A key string of 31 bytes, 33 bytes or none is malformed. */
	build_blob(wire, "ssh-ed25519", t1_public, 31);
	CHECK_EQ(ssh_key_parse_public_blob(wire->data, wire->size, parsed), EINVAL);
	build_blob(wire, "ssh-ed25519", big_key, 33);
	CHECK_EQ(ssh_key_parse_public_blob(wire->data, wire->size, parsed), EINVAL);
	build_blob(wire, "ssh-ed25519", t1_public, 0);
	CHECK_EQ(ssh_key_parse_public_blob(wire->data, wire->size, parsed), EINVAL);

	/* A byte after the blob is malformed. */
	build_blob(wire, "ssh-ed25519", t1_public, SSH_ED25519_PUBLIC);
	wire_u8(wire, 0);
	CHECK_EQ(ssh_key_parse_public_blob(wire->data, wire->size, parsed), EINVAL);

	/* A string that claims more bytes than there are is malformed. */
	wire_reset(wire);
	wire_text(wire, "ssh-ed25519");
	wire_u32(wire, 0xffffffffU);
	wire_bytes(wire, t1_public, SSH_ED25519_PUBLIC);
	CHECK_EQ(ssh_key_parse_public_blob(wire->data, wire->size, parsed), EINVAL);
	wire_reset(wire);
	wire_u32(wire, 0xffffffffU);
	wire_bytes(wire, "ssh-ed25519", 11);
	CHECK_EQ(ssh_key_parse_public_blob(wire->data, wire->size, parsed), EINVAL);

	/* Every cut of a good blob is malformed. */
	build_blob(wire, "ssh-ed25519", t1_public, SSH_ED25519_PUBLIC);
	for (length = 0; length < SSH_KEY_BLOB_SIZE; length++) {
		memset(parsed, 0xa5, sizeof(parsed));
		error = ssh_key_parse_public_blob(wire->data, length, parsed);
		CHECK_EQ(error, EINVAL);
		CHECK_BYTES(parsed, sentinel, sizeof(parsed));
	}

	/* No bytes at all, with a NULL pointer, is malformed too. */
	CHECK_EQ(ssh_key_parse_public_blob(NULL, 0, parsed), EINVAL);
	CHECK_EQ(ssh_key_parse_public_blob(NULL, 5, parsed), EINVAL);
}

/* Checks signing and verifying with the RFC 8032 keys, and every way a signature blob can be wrong. */
static void
test_sign_verify(void)
{
	struct ssh_key key;
	struct ssh_key public_only;
	struct ssh_buf out;
	struct wire *wire;
	uint8_t blob[SSH_KEY_SIGNATURE_BLOB_SIZE];
	uint8_t copy[SSH_KEY_SIGNATURE_BLOB_SIZE + 1U];
	uint8_t message[2];
	uint8_t big[SSH_ED25519_SIGNATURE + 1U];
	size_t index;
	unsigned bit;
	int error;

	/* Makes a signature one byte too long. */
	wire = &outer_wire;
	memset(big, 0x33, sizeof(big));

	/* TEST 1: the empty message signs to the RFC's signature inside a blob of 83 bytes. */
	make_key(&key, t1_seed, t1_public, "t1");
	build_signature(wire, "ssh-ed25519", t1_signature, SSH_ED25519_SIGNATURE);
	CHECK_EQ(wire->size, SSH_KEY_SIGNATURE_BLOB_SIZE);
	ssh_buf_init(&out, 256, 0);
	error = ssh_key_sign(&key, NULL, 0, &out);
	CHECK_EQ(error, 0);
	CHECK_EQ(out.size, SSH_KEY_SIGNATURE_BLOB_SIZE);
	CHECK_BYTES(out.data, wire->data, SSH_KEY_SIGNATURE_BLOB_SIZE);
	memcpy(blob, out.data, sizeof(blob));
	ssh_buf_free(&out);

	/* The blob verifies for the empty message with the public key. */
	CHECK_EQ(ssh_key_verify(t1_public, NULL, 0, blob, sizeof(blob)), 0);

	/* TEST 2: the one-byte message. */
	make_key(&key, t2_seed, t2_public, "t2");
	build_signature(wire, "ssh-ed25519", t2_signature, SSH_ED25519_SIGNATURE);
	ssh_buf_init(&out, 256, 0);
	error = ssh_key_sign(&key, t2_message, sizeof(t2_message), &out);
	CHECK_EQ(error, 0);
	CHECK_EQ(out.size, SSH_KEY_SIGNATURE_BLOB_SIZE);
	CHECK_BYTES(out.data, wire->data, SSH_KEY_SIGNATURE_BLOB_SIZE);
	CHECK_EQ(ssh_key_verify(t2_public, t2_message, sizeof(t2_message), out.data, out.size), 0);
	ssh_buf_free(&out);

	/* Signing appends after what the buffer holds. */
	ssh_buf_init(&out, 256, 0);
	ssh_buf_put_u8(&out, 0x99);
	error = ssh_key_sign(&key, t2_message, sizeof(t2_message), &out);
	CHECK_EQ(error, 0);
	CHECK_EQ(out.size, 1U + SSH_KEY_SIGNATURE_BLOB_SIZE);
	CHECK_EQ(out.data[0], 0x99);
	CHECK_BYTES(out.data + 1, wire->data, SSH_KEY_SIGNATURE_BLOB_SIZE);
	ssh_buf_free(&out);

	/* A key without a seed does not sign and appends nothing. */
	memset(&public_only, 0, sizeof(public_only));
	memcpy(public_only.public_key, t2_public, SSH_ED25519_PUBLIC);
	ssh_buf_init(&out, 256, 0);
	error = ssh_key_sign(&public_only, t2_message, sizeof(t2_message), &out);
	CHECK_EQ(error, EINVAL);
	CHECK_EQ(out.size, 0);
	ssh_buf_free(&out);

	/* A buffer that cannot take 83 bytes gets nothing, and the error is the buffer's. */
	ssh_buf_init(&out, 60, 0);
	error = ssh_key_sign(&key, t2_message, sizeof(t2_message), &out);
	CHECK_EQ(error, EMSGSIZE);
	CHECK_EQ(out.size, 0);
	ssh_buf_free(&out);

	/* The signature does not verify for another message, another key, or an empty message. */
	build_signature(wire, "ssh-ed25519", t2_signature, SSH_ED25519_SIGNATURE);
	message[0] = 0x73;
	CHECK_EQ(ssh_key_verify(t2_public, message, 1, wire->data, wire->size), EBADMSG);
	message[0] = 0x72;
	message[1] = 0x00;
	CHECK_EQ(ssh_key_verify(t2_public, message, 2, wire->data, wire->size), EBADMSG);
	CHECK_EQ(ssh_key_verify(t2_public, NULL, 0, wire->data, wire->size), EBADMSG);
	CHECK_EQ(ssh_key_verify(t1_public, t2_message, 1, wire->data, wire->size), EBADMSG);
	CHECK_EQ(ssh_key_verify(t2_public, t2_message, 1, wire->data, wire->size), 0);

	/* Another algorithm name is ENOTSUP. */
	build_signature(wire, "ssh-rsa", t2_signature, SSH_ED25519_SIGNATURE);
	CHECK_EQ(ssh_key_verify(t2_public, t2_message, 1, wire->data, wire->size), ENOTSUP);
	build_signature(wire, "ssh-ed25518", t2_signature, SSH_ED25519_SIGNATURE);
	CHECK_EQ(ssh_key_verify(t2_public, t2_message, 1, wire->data, wire->size), ENOTSUP);

	/* A signature string of 63 or 65 bytes or a missing one is malformed (EINVAL, not EBADMSG). */
	build_signature(wire, "ssh-ed25519", t2_signature, 63);
	CHECK_EQ(ssh_key_verify(t2_public, t2_message, 1, wire->data, wire->size), EINVAL);
	build_signature(wire, "ssh-ed25519", big, 65);
	CHECK_EQ(ssh_key_verify(t2_public, t2_message, 1, wire->data, wire->size), EINVAL);
	build_signature(wire, "ssh-ed25519", t2_signature, 0);
	CHECK_EQ(ssh_key_verify(t2_public, t2_message, 1, wire->data, wire->size), EINVAL);

	/* A byte after the blob is malformed. */
	build_signature(wire, "ssh-ed25519", t2_signature, SSH_ED25519_SIGNATURE);
	wire_u8(wire, 0);
	CHECK_EQ(ssh_key_verify(t2_public, t2_message, 1, wire->data, wire->size), EINVAL);

	/* Every cut of a good blob is malformed. */
	build_signature(wire, "ssh-ed25519", t2_signature, SSH_ED25519_SIGNATURE);
	for (index = 0; index < SSH_KEY_SIGNATURE_BLOB_SIZE; index++)
		CHECK_EQ(ssh_key_verify(t2_public, t2_message, 1, wire->data, index), EINVAL);

	/* Missing bytes are malformed. */
	CHECK_EQ(ssh_key_verify(t2_public, t2_message, 1, NULL, 0), EINVAL);
	CHECK_EQ(ssh_key_verify(t2_public, t2_message, 1, NULL, 9), EINVAL);

	/* A change of any one bit of the blob makes it fail: in the signature bytes as a bad signature. */
	memcpy(copy, wire->data, SSH_KEY_SIGNATURE_BLOB_SIZE);
	for (index = 0; index < SSH_KEY_SIGNATURE_BLOB_SIZE; index++) {
		for (bit = 0; bit < 8; bit++) {
			copy[index] ^= (uint8_t)(1U << bit);
			error = ssh_key_verify(t2_public, t2_message, 1, copy, SSH_KEY_SIGNATURE_BLOB_SIZE);
			CHECK(error != 0);
			if (index >= 19U)
				CHECK_EQ(error, EBADMSG);
			copy[index] ^= (uint8_t)(1U << bit);
		}
	}

	/* Every bit was put back, so the copy equals the original blob again. */
	CHECK_BYTES(copy, wire->data, SSH_KEY_SIGNATURE_BLOB_SIZE);

	/* Wipes both keys. */
	ssh_key_clear(&key);
	ssh_key_clear(&public_only);
}

/* Checks the one-line public key form: writing, reading, and every deviation. */
static void
test_public_line(void)
{
	struct ssh_buf out;
	struct ssh_key key;
	struct ssh_key sentinel;
	struct wire *wire;
	char line[1024];
	char other[256];
	char long_comment[400];
	char expected[512];
	char rsa_base64[256];
	size_t length;
	size_t index;
	int error;
	size_t expected_length;
	size_t line_length;

	/* Makes a key filled with a known byte, to see whether a failed parse writes it. */
	wire = &outer_wire;
	memset(&sentinel, 0x5a, sizeof(sentinel));

	/* The base64 worked out here is the one worked out by hand. */
	build_blob(wire, "ssh-ed25519", t1_public, SSH_ED25519_PUBLIC);
	test_base64(wire->data, wire->size, other, 1);
	CHECK(strcmp(other, t1_blob_base64) == 0);

	/* A line with a comment: type, a space, the base64, a space, the comment, LF. */
	ssh_buf_init(&out, 1024, 0);
	ssh_key_put_public_line(t1_public, "alice@example", &out);
	CHECK_EQ(ssh_buf_error(&out), 0);
	strcpy(expected, "ssh-ed25519 ");
	strcat(expected, t1_blob_base64);
	strcat(expected, " alice@example\n");
	CHECK_EQ(out.size, strlen(expected));
	CHECK_BYTES(out.data, expected, strlen(expected));
	ssh_buf_free(&out);

	/* No comment (NULL, and empty) leaves out the space. */
	strcpy(expected, "ssh-ed25519 ");
	strcat(expected, t1_blob_base64);
	strcat(expected, "\n");
	ssh_buf_init(&out, 1024, 0);
	ssh_key_put_public_line(t1_public, NULL, &out);
	CHECK_EQ(out.size, strlen(expected));
	CHECK_BYTES(out.data, expected, strlen(expected));
	ssh_buf_free(&out);
	ssh_buf_init(&out, 1024, 0);
	ssh_key_put_public_line(t1_public, "", &out);
	CHECK_EQ(out.size, strlen(expected));
	CHECK_BYTES(out.data, expected, strlen(expected));
	ssh_buf_free(&out);

	/* A comment with an LF or a CR would end the line early: EINVAL recorded, nothing appended. */
	ssh_buf_init(&out, 1024, 0);
	ssh_key_put_public_line(t1_public, "a\nb", &out);
	CHECK_EQ(ssh_buf_error(&out), EINVAL);
	CHECK_EQ(out.size, 0);
	ssh_buf_free(&out);
	ssh_buf_init(&out, 1024, 0);
	ssh_key_put_public_line(t1_public, "a\rb", &out);
	CHECK_EQ(ssh_buf_error(&out), EINVAL);
	CHECK_EQ(out.size, 0);
	ssh_buf_free(&out);

	/* A buffer that is too small records the error. */
	ssh_buf_init(&out, 30, 0);
	ssh_key_put_public_line(t1_public, "alice", &out);
	CHECK_EQ(ssh_buf_error(&out), EMSGSIZE);
	ssh_buf_free(&out);

	/* A line written reads back, with the comment. */
	ssh_buf_init(&out, 1024, 0);
	ssh_key_put_public_line(t2_public, "bob key", &out);
	CHECK_EQ(ssh_buf_error(&out), 0);
	error = ssh_key_parse_public_line((const char *)out.data, out.size - 1U, &key);
	CHECK_EQ(error, 0);
	CHECK_BYTES(key.public_key, t2_public, SSH_ED25519_PUBLIC);
	CHECK_EQ(key.private, 0);
	CHECK(strcmp(key.comment, "bob key") == 0);
	ssh_buf_free(&out);

	/* One space, a comment. */
	strcpy(line, "ssh-ed25519 ");
	strcat(line, t1_blob_base64);
	strcat(line, " comment");
	memset(&key, 0x5a, sizeof(key));
	error = ssh_key_parse_public_line(line, strlen(line), &key);
	CHECK_EQ(error, 0);
	CHECK_BYTES(key.public_key, t1_public, SSH_ED25519_PUBLIC);
	CHECK_EQ(key.private, 0);
	CHECK(strcmp(key.comment, "comment") == 0);

	/* No comment. */
	strcpy(line, "ssh-ed25519 ");
	strcat(line, t1_blob_base64);
	error = ssh_key_parse_public_line(line, strlen(line), &key);
	CHECK_EQ(error, 0);
	CHECK_BYTES(key.public_key, t1_public, SSH_ED25519_PUBLIC);
	CHECK_EQ(key.comment[0], 0);

	/* Tabs between the fields; the comment has an inner space. */
	strcpy(line, "ssh-ed25519\t");
	strcat(line, t1_blob_base64);
	strcat(line, "\tmy comment");
	error = ssh_key_parse_public_line(line, strlen(line), &key);
	CHECK_EQ(error, 0);
	CHECK(strcmp(key.comment, "my comment") == 0);

	/* Many spaces and tabs; trailing white space is not in the comment, the inner spaces are kept. */
	strcpy(line, "ssh-ed25519   \t ");
	strcat(line, t1_blob_base64);
	strcat(line, "    a  b \t ");
	error = ssh_key_parse_public_line(line, strlen(line), &key);
	CHECK_EQ(error, 0);
	CHECK(strcmp(key.comment, "a  b") == 0);

	/* Leading white space is skipped. */
	strcpy(line, "  \tssh-ed25519 ");
	strcat(line, t1_blob_base64);
	strcat(line, " c");
	error = ssh_key_parse_public_line(line, strlen(line), &key);
	CHECK_EQ(error, 0);
	CHECK(strcmp(key.comment, "c") == 0);

	/* A CR at the end of the line (a CR LF file whose LF was cut) is not content. */
	strcpy(line, "ssh-ed25519 ");
	strcat(line, t1_blob_base64);
	strcat(line, " c\r");
	error = ssh_key_parse_public_line(line, strlen(line), &key);
	CHECK_EQ(error, 0);
	CHECK(strcmp(key.comment, "c") == 0);
	strcpy(line, "ssh-ed25519 ");
	strcat(line, t1_blob_base64);
	strcat(line, "\r");
	error = ssh_key_parse_public_line(line, strlen(line), &key);
	CHECK_EQ(error, 0);
	CHECK_EQ(key.comment[0], 0);

	/* A comment too long is cut to what fits. */
	memset(long_comment, 'x', sizeof(long_comment) - 1U);
	long_comment[sizeof(long_comment) - 1U] = '\0';
	strcpy(line, "ssh-ed25519 ");
	strcat(line, t1_blob_base64);
	strcat(line, " ");
	strcat(line, long_comment);
	error = ssh_key_parse_public_line(line, strlen(line), &key);
	CHECK_EQ(error, 0);
	CHECK_EQ(strlen(key.comment), SSH_KEY_COMMENT_MAX - 1U);

	/* Comments around the limit: up to 255 bytes are kept whole, longer ones are cut to 255. */
	for (length = 254; length <= 257U; length++) {
		memset(long_comment, 'y', length);
		long_comment[length] = '\0';
		strcpy(line, "ssh-ed25519 ");
		strcat(line, t1_blob_base64);
		strcat(line, " ");
		strcat(line, long_comment);
		error = ssh_key_parse_public_line(line, strlen(line), &key);
		CHECK_EQ(error, 0);
		expected_length = length;
		if (expected_length > 255U)
			expected_length = 255U;
		CHECK_EQ(strlen(key.comment), expected_length);
	}

	/* Another type in the type field is ENOTSUP, and the output is not written. */
	strcpy(line, "ssh-rsa ");
	strcat(line, t1_blob_base64);
	strcat(line, " c");
	key = sentinel;
	error = ssh_key_parse_public_line(line, strlen(line), &key);
	CHECK_EQ(error, ENOTSUP);
	CHECK_BYTES(&key, &sentinel, sizeof(key));
	strcpy(line, "SSH-ED25519 ");
	strcat(line, t1_blob_base64);
	error = ssh_key_parse_public_line(line, strlen(line), &key);
	CHECK_EQ(error, ENOTSUP);

	/* The blob names another type than the field does: malformed. */
	build_blob(wire, "ssh-rsa", t1_public, SSH_ED25519_PUBLIC);
	test_base64(wire->data, wire->size, rsa_base64, 1);
	strcpy(line, "ssh-ed25519 ");
	strcat(line, rsa_base64);
	key = sentinel;
	error = ssh_key_parse_public_line(line, strlen(line), &key);
	CHECK_EQ(error, EINVAL);
	CHECK_BYTES(&key, &sentinel, sizeof(key));

	/* A blob of a key string of 31 bytes and one with a trailing byte are malformed. */
	build_blob(wire, "ssh-ed25519", t1_public, 31);
	test_base64(wire->data, wire->size, other, 1);
	strcpy(line, "ssh-ed25519 ");
	strcat(line, other);
	CHECK_EQ(ssh_key_parse_public_line(line, strlen(line), &key), EINVAL);
	build_blob(wire, "ssh-ed25519", t1_public, SSH_ED25519_PUBLIC);
	wire_u8(wire, 0);
	test_base64(wire->data, wire->size, other, 1);
	strcpy(line, "ssh-ed25519 ");
	strcat(line, other);
	CHECK_EQ(ssh_key_parse_public_line(line, strlen(line), &key), EINVAL);

	/* Base64 errors: a character outside the alphabet, a missing last character, a blob longer than any key. */
	strcpy(line, "ssh-ed25519 ");
	strcat(line, t1_blob_base64);
	line[20] = '*';
	CHECK_EQ(ssh_key_parse_public_line(line, strlen(line), &key), EINVAL);
	strcpy(line, "ssh-ed25519 ");
	strcat(line, t1_blob_base64);
	line[strlen(line) - 1U] = '\0';
	CHECK_EQ(ssh_key_parse_public_line(line, strlen(line), &key), EINVAL);
	strcpy(line, "ssh-ed25519 ");
	strcat(line, t1_blob_base64);
	strcat(line, "AAAA");
	CHECK_EQ(ssh_key_parse_public_line(line, strlen(line), &key), EINVAL);
	strcpy(line, "ssh-ed25519 ");
	strcat(line, t1_blob_base64);
	line[strlen(line) - 4U] = '\0';
	CHECK_EQ(ssh_key_parse_public_line(line, strlen(line), &key), EINVAL);

	/* A line without a blob is malformed, however much white space it has. */
	CHECK_EQ(ssh_key_parse_public_line("ssh-ed25519", 11, &key), EINVAL);
	CHECK_EQ(ssh_key_parse_public_line("ssh-ed25519 ", 12, &key), EINVAL);
	CHECK_EQ(ssh_key_parse_public_line("ssh-ed25519 \t \r", 15, &key), EINVAL);
	CHECK_EQ(ssh_key_parse_public_line("", 0, &key), EINVAL);
	CHECK_EQ(ssh_key_parse_public_line("   ", 3, &key), EINVAL);
	CHECK_EQ(ssh_key_parse_public_line(NULL, 0, &key), EINVAL);

	/* A NUL or an LF inside the text is malformed. */
	strcpy(line, "ssh-ed25519 ");
	strcat(line, t1_blob_base64);
	length = strlen(line);
	line[length] = '\0';
	line[length + 1U] = 'x';
	CHECK_EQ(ssh_key_parse_public_line(line, length + 2U, &key), EINVAL);
	line[length] = '\n';
	line[length + 1U] = 'x';
	CHECK_EQ(ssh_key_parse_public_line(line, length + 2U, &key), EINVAL);
	line[length] = ' ';
	line[length + 1U] = 'x';
	line[length + 2U] = '\0';
	CHECK_EQ(ssh_key_parse_public_line(line, length + 2U, &key), 0);

	/* Every cut of a good line either is a good key or is refused; none crashes, none writes key on an error. */
	strcpy(line, "ssh-ed25519 ");
	strcat(line, t1_blob_base64);
	strcat(line, " tail");
	line_length = strlen(line);
	for (index = 0; index <= line_length; index++) {
		key = sentinel;
		error = ssh_key_parse_public_line(line, index, &key);
		if (error != 0)
			CHECK_BYTES(&key, &sentinel, sizeof(key));
		else
			CHECK_BYTES(key.public_key, t1_public, SSH_ED25519_PUBLIC);
	}
}

/* Checks reading a good private key file in its allowed forms. */
static void
test_private_good(void)
{
	struct file_spec spec;
	struct ssh_key key;
	size_t length;
	size_t count;
	char comment[32];
	int error;
	size_t expected_length;

	/* The plain file: TEST 1 key, comment "ab", 70-character lines. */
	spec_init(&spec);
	memset(&key, 0x5a, sizeof(key));
	error = parse_spec(&spec, &key);
	CHECK_EQ(error, 0);
	CHECK_EQ(key.private, 1);
	CHECK_BYTES(key.seed, t1_seed, SSH_ED25519_SEED);
	CHECK_BYTES(key.public_key, t1_public, SSH_ED25519_PUBLIC);
	CHECK(strcmp(key.comment, "ab") == 0);

	/* The file has the BEGIN line first, lines of 70 characters, and the END line last. */
	length = build_text(&spec);
	CHECK(memcmp(file_text, SSH_KEY_PRIVATE_BEGIN "\n", strlen(SSH_KEY_PRIVATE_BEGIN) + 1U) == 0);
	CHECK(length > strlen(SSH_KEY_PRIVATE_END) + 1U);
	CHECK(memcmp(file_text + length - strlen(SSH_KEY_PRIVATE_END) - 1U, SSH_KEY_PRIVATE_END "\n",
	    strlen(SSH_KEY_PRIVATE_END) + 1U) == 0);

	/* The TEST 2 key. */
	spec_init(&spec);
	spec.blob_key = t2_public;
	spec.inner_public = t2_public;
	spec.pair_seed = t2_seed;
	spec.pair_public = t2_public;
	error = parse_spec(&spec, &key);
	CHECK_EQ(error, 0);
	CHECK_BYTES(key.seed, t2_seed, SSH_ED25519_SEED);
	CHECK_BYTES(key.public_key, t2_public, SSH_ED25519_PUBLIC);

	/* Lines ending with CR LF. */
	spec_init(&spec);
	spec.crlf = 1;
	error = parse_spec(&spec, &key);
	CHECK_EQ(error, 0);
	CHECK_BYTES(key.seed, t1_seed, SSH_ED25519_SEED);

	/* Extra lines before and after, the way a file with a header or a trailer looks. */
	spec_init(&spec);
	spec.prefix = "Comment: not part of the key\n\n";
	spec.suffix = "trailing text\nmore\n";
	error = parse_spec(&spec, &key);
	CHECK_EQ(error, 0);
	CHECK_BYTES(key.seed, t1_seed, SSH_ED25519_SEED);

	/* No line feed after the END line. */
	spec_init(&spec);
	spec.end_newline = 0;
	error = parse_spec(&spec, &key);
	CHECK_EQ(error, 0);

	/* Other line widths, and the base64 on one line. */
	spec_init(&spec);
	spec.wrap = 64;
	CHECK_EQ(parse_spec(&spec, &key), 0);
	spec.wrap = 1;
	CHECK_EQ(parse_spec(&spec, &key), 0);
	spec.wrap = WRAP_ONE_LINE;
	CHECK_EQ(parse_spec(&spec, &key), 0);

	/* Every padding length (0 to 7) works: comments of 0 to 20 bytes. */
	for (count = 0; count <= 20U; count++) {
		memset(comment, 'k', count);
		comment[count] = '\0';
		spec_init(&spec);
		spec.comment = comment;
		spec.comment_size = count;
		memset(&key, 0x5a, sizeof(key));
		error = parse_spec(&spec, &key);
		CHECK_EQ(error, 0);
		CHECK(strcmp(key.comment, comment) == 0);
		CHECK_BYTES(key.seed, t1_seed, SSH_ED25519_SEED);
	}

	/* Comments around the limit and far above it: up to 255 bytes are kept whole, longer ones are cut to 255. */
	memset(edge_comment, 'e', sizeof(edge_comment));
	for (count = 254; count <= 257U; count++) {
		spec_init(&spec);
		spec.comment = edge_comment;
		spec.comment_size = count;
		error = parse_spec(&spec, &key);
		CHECK_EQ(error, 0);
		expected_length = count;
		if (expected_length > 255U)
			expected_length = 255U;
		CHECK_EQ(strlen(key.comment), expected_length);
	}

	/* A comment far above the limit is cut to the largest size the key keeps. */
	spec_init(&spec);
	spec.comment = edge_comment;
	spec.comment_size = 300;
	error = parse_spec(&spec, &key);
	CHECK_EQ(error, 0);
	CHECK_EQ(strlen(key.comment), SSH_KEY_COMMENT_MAX - 1U);

	/* Different check integers in different files are fine while the two in one file agree. */
	spec_init(&spec);
	spec.check_first = 0;
	spec.check_second = 0;
	CHECK_EQ(parse_spec(&spec, &key), 0);
	spec.check_first = 0xffffffffU;
	spec.check_second = 0xffffffffU;
	CHECK_EQ(parse_spec(&spec, &key), 0);
}

/* Checks the private key files that must be refused, each with the error the header names. */
static void
test_private_bad(void)
{
	struct file_spec spec;
	struct ssh_key key;
	static uint8_t short_key[SSH_ED25519_PUBLIC];
	char big_comment[8];

	/* Makes a public key of the wrong size, filled with a known byte. */
	memset(short_key, 0x44, sizeof(short_key));

	/* The check integers differ. */
	spec_init(&spec);
	spec.check_second = spec.check_first + 1U;
	memset(&key, 0x5a, sizeof(key));
	CHECK_EQ(parse_spec(&spec, &key), EINVAL);
	CHECK(key_is_zero(&key));

	/* The padding is wrong: 1,2,4 instead of 1,2,3 (a 2-byte comment leaves three bytes of padding). */
	spec_init(&spec);
	spec.padding = PAD_LAST_WRONG;
	memset(&key, 0x5a, sizeof(key));
	CHECK_EQ(parse_spec(&spec, &key), EINVAL);
	CHECK(key_is_zero(&key));

	/* The padding is zeros, one byte short, or a whole block too long. */
	spec_init(&spec);
	spec.padding = PAD_ZEROS;
	CHECK_EQ(parse_spec(&spec, &key), EINVAL);
	spec_init(&spec);
	spec.padding = PAD_SHORT;
	CHECK_EQ(parse_spec(&spec, &key), EINVAL);
	spec_init(&spec);
	spec.padding = PAD_EXTRA_BLOCK;
	CHECK_EQ(parse_spec(&spec, &key), EINVAL);

	/* A section already a multiple of 8 (a 5-byte comment) followed by a whole block of padding. */
	spec_init(&spec);
	spec.comment = "abcde";
	spec.comment_size = 5;
	CHECK_EQ(parse_spec(&spec, &key), 0);
	spec.padding = PAD_EXTRA_BLOCK;
	CHECK_EQ(parse_spec(&spec, &key), EINVAL);

	/* An encrypted file: cipher aes256-ctr with the bcrypt KDF and its options (PROTOCOL.key 5). */
	spec_init(&spec);
	spec.cipher = "aes256-ctr";
	spec.kdf = "bcrypt";
	spec.options_size = 16;
	memset(&key, 0x5a, sizeof(key));
	CHECK_EQ(parse_spec(&spec, &key), ENOTSUP);
	CHECK(key_is_zero(&key));

	/* A cipher alone, and a KDF alone. */
	spec_init(&spec);
	spec.cipher = "aes256-ctr";
	CHECK_EQ(parse_spec(&spec, &key), ENOTSUP);
	spec_init(&spec);
	spec.kdf = "bcrypt";
	CHECK_EQ(parse_spec(&spec, &key), ENOTSUP);

	/* KDF options with the KDF "none" are malformed. */
	spec_init(&spec);
	spec.options_size = 4;
	CHECK_EQ(parse_spec(&spec, &key), EINVAL);

	/* A file of two keys, and of none. */
	spec_init(&spec);
	spec.count = 2;
	CHECK_EQ(parse_spec(&spec, &key), ENOTSUP);
	spec_init(&spec);
	spec.count = 0;
	CHECK_EQ(parse_spec(&spec, &key), ENOTSUP);

	/* The public key blob is another type: not this key. */
	spec_init(&spec);
	spec.blob_type = "ssh-rsa";
	CHECK_EQ(parse_spec(&spec, &key), ENOTSUP);

	/* The blob's key string is 31 bytes. */
	spec_init(&spec);
	spec.blob_key = short_key;
	spec.blob_key_size = 31;
	CHECK_EQ(parse_spec(&spec, &key), EINVAL);

	/* The blob holds another key than the private key. */
	spec_init(&spec);
	spec.blob_key = t2_public;
	memset(&key, 0x5a, sizeof(key));
	CHECK_EQ(parse_spec(&spec, &key), EINVAL);
	CHECK(key_is_zero(&key));

	/* The public key of the private key differs from the blob's. */
	spec_init(&spec);
	spec.inner_public = t2_public;
	CHECK_EQ(parse_spec(&spec, &key), EINVAL);

	/* The second half of the 64 bytes differs from the public key. */
	spec_init(&spec);
	spec.pair_public = t2_public;
	CHECK_EQ(parse_spec(&spec, &key), EINVAL);

	/* The seed is that of another key: its public key is not the one in the file. */
	spec_init(&spec);
	spec.pair_seed = t2_seed;
	memset(&key, 0x5a, sizeof(key));
	CHECK_EQ(parse_spec(&spec, &key), EINVAL);
	CHECK(key_is_zero(&key));

	/* The key type in the private section differs from the blob's. */
	spec_init(&spec);
	spec.inner_type = "ssh-rsa";
	CHECK_EQ(parse_spec(&spec, &key), EINVAL);

	/* The private public key string is 31 bytes; the pair is 63 bytes. */
	spec_init(&spec);
	spec.inner_public = short_key;
	spec.inner_public_size = 31;
	CHECK_EQ(parse_spec(&spec, &key), EINVAL);
	spec_init(&spec);
	spec.pair_size = SSH_ED25519_SEED + SSH_ED25519_PUBLIC - 1U;
	CHECK_EQ(parse_spec(&spec, &key), EINVAL);

	/* A comment with a NUL inside cannot be kept. */
	memset(big_comment, 'a', sizeof(big_comment));
	big_comment[3] = '\0';
	spec_init(&spec);
	spec.comment = big_comment;
	spec.comment_size = sizeof(big_comment);
	CHECK_EQ(parse_spec(&spec, &key), EINVAL);

	/* The magic is wrong: one byte changed, and the NUL replaced. */
	spec_init(&spec);
	spec.magic[0] = 'X';
	CHECK_EQ(parse_spec(&spec, &key), EINVAL);
	spec_init(&spec);
	spec.magic[14] = '!';
	CHECK_EQ(parse_spec(&spec, &key), EINVAL);

	/* Bytes after the private section. */
	spec_init(&spec);
	spec.outer_extra = 1;
	CHECK_EQ(parse_spec(&spec, &key), EINVAL);

	/* A decoded file that ends early (the last byte, and the whole private section's tail). */
	spec_init(&spec);
	spec.outer_cut = 1;
	CHECK_EQ(parse_spec(&spec, &key), EINVAL);
	spec.outer_cut = 40;
	CHECK_EQ(parse_spec(&spec, &key), EINVAL);
}

/* Checks the armour around the base64: missing or wrong lines, bad base64, size limits, cuts. */
static void
test_private_framing(void)
{
	struct file_spec spec;
	struct ssh_key key;
	size_t length;
	size_t cut;
	int error;

	/* No BEGIN line. */
	spec_init(&spec);
	spec.omit_begin = 1;
	memset(&key, 0x5a, sizeof(key));
	CHECK_EQ(parse_spec(&spec, &key), EINVAL);
	CHECK(key_is_zero(&key));

	/* No END line. */
	spec_init(&spec);
	spec.omit_end = 1;
	memset(&key, 0x5a, sizeof(key));
	CHECK_EQ(parse_spec(&spec, &key), EINVAL);
	CHECK(key_is_zero(&key));

	/* Another armour (an RSA file). */
	spec_init(&spec);
	spec.begin_line = "-----BEGIN RSA PRIVATE KEY-----";
	CHECK_EQ(parse_spec(&spec, &key), EINVAL);

	/* An empty line in the middle of the base64. */
	spec_init(&spec);
	spec.blank_line = 1;
	CHECK_EQ(parse_spec(&spec, &key), EINVAL);

	/* Nothing between the BEGIN and the END lines. */
	length = (size_t)0;
	strcpy(file_text, SSH_KEY_PRIVATE_BEGIN "\n" SSH_KEY_PRIVATE_END "\n");
	length = strlen(file_text);
	memset(&key, 0x5a, sizeof(key));
	CHECK_EQ(ssh_key_parse_private(file_text, length, &key), EINVAL);
	CHECK(key_is_zero(&key));

	/* A character outside the base64 alphabet in the body. */
	spec_init(&spec);
	length = build_text(&spec);
	file_text[strlen(SSH_KEY_PRIVATE_BEGIN) + 1U + 10U] = '*';
	memset(&key, 0x5a, sizeof(key));
	CHECK_EQ(ssh_key_parse_private(file_text, length, &key), EINVAL);
	CHECK(key_is_zero(&key));

	/* Text that is not a key file at all, and no text. */
	CHECK_EQ(ssh_key_parse_private("hello\n", 6, &key), EINVAL);
	CHECK_EQ(ssh_key_parse_private("", 0, &key), EINVAL);
	CHECK_EQ(ssh_key_parse_private(NULL, 0, &key), EINVAL);

	/* A file larger than the limit is EMSGSIZE; one of exactly the limit is looked at (and refused as no key). */
	memset(file_text, 'x', sizeof(file_text));
	memset(&key, 0x5a, sizeof(key));
	CHECK_EQ(ssh_key_parse_private(file_text, SSH_KEY_FILE_MAX + 1U, &key), EMSGSIZE);
	CHECK(key_is_zero(&key));
	CHECK_EQ(ssh_key_parse_private(file_text, SSH_KEY_FILE_MAX, &key), EINVAL);

	/* Every cut of a good file is refused, except the one that only drops the final line feed. */
	spec_init(&spec);
	length = build_text(&spec);
	for (cut = 0; cut < length; cut++) {
		memset(&key, 0x5a, sizeof(key));
		error = ssh_key_parse_private(file_text, cut, &key);
		if (cut == length - 1U) {
			CHECK_EQ(error, 0);
		} else {
			CHECK_EQ(error, EINVAL);
			CHECK(key_is_zero(&key));
		}
	}
}

/* Checks ssh_key_put_private: the bytes written, the round trip, and the refusals. */
static void
test_private_put(void)
{
	struct file_spec spec;
	struct ssh_key key;
	struct ssh_key back;
	struct ssh_buf out;
	char comment[300];
	size_t length;
	size_t count;
	size_t line;
	size_t position;
	size_t width;
	int error;
	int different;
	int is_wrapped_line;

	/* The bytes written equal the file built apart from the module, for every padding length. */
	for (count = 0; count <= 20U; count++) {
		memset(comment, 'q', count);
		comment[count] = '\0';
		make_key(&key, t1_seed, t1_public, comment);
		spec_init(&spec);
		spec.comment = comment;
		spec.comment_size = count;
		spec.check_first = 0xdeadbeefU;
		spec.check_second = 0xdeadbeefU;
		length = build_text(&spec);
		ssh_buf_init(&out, SSH_KEY_FILE_MAX, 1);
		error = ssh_key_put_private(&key, 0xdeadbeefU, &out);
		CHECK_EQ(error, 0);
		CHECK_EQ(out.size, length);
		if (out.size == length)
			CHECK_BYTES(out.data, file_text, length);

		/* It reads back as the same key. */
		memset(&back, 0x5a, sizeof(back));
		error = ssh_key_parse_private((const char *)out.data, out.size, &back);
		CHECK_EQ(error, 0);
		CHECK_BYTES(&back, &key, sizeof(key));
		ssh_buf_free(&out);
	}

	/* A TEST 2 key with a long comment: every line but the last is 70 characters and each ends with LF. */
	memset(comment, 'w', 200);
	comment[200] = '\0';
	make_key(&key, t2_seed, t2_public, comment);
	ssh_buf_init(&out, SSH_KEY_FILE_MAX, 1);
	error = ssh_key_put_private(&key, 7, &out);
	CHECK_EQ(error, 0);
	position = 0;
	line = 0;
	width = 0;
	while (position < out.size) {
		width = 0;
		while (position + width < out.size && out.data[position + width] != '\n')
			width++;
		CHECK(position + width < out.size);
		is_wrapped_line = 0;
		if (line > 0 && position + width + 1U < out.size && out.data[position + width + 1U] != '-') {
			different = memcmp(out.data + position, "-----END", 8);
			if (different != 0)
				is_wrapped_line = 1;
		}

		/* Every line before the last one of the base64 is wrapped at the same width. */
		if (is_wrapped_line != 0)
			CHECK_EQ(width, SSH_KEY_PRIVATE_WRAP);
		position += width + 1U;
		line++;
	}

	/* The key must span several lines, and read back as the same key. */
	CHECK(line > 3U);
	memset(&back, 0x5a, sizeof(back));
	error = ssh_key_parse_private((const char *)out.data, out.size, &back);
	CHECK_EQ(error, 0);
	CHECK_BYTES(&back, &key, sizeof(key));
	ssh_buf_free(&out);

	/* A key without a seed is not written. */
	memset(&key, 0, sizeof(key));
	memcpy(key.public_key, t1_public, SSH_ED25519_PUBLIC);
	ssh_buf_init(&out, SSH_KEY_FILE_MAX, 1);
	CHECK_EQ(ssh_key_put_private(&key, 1, &out), EINVAL);
	CHECK_EQ(out.size, 0);
	ssh_buf_free(&out);

	/* A buffer that cannot hold the file gets nothing. */
	make_key(&key, t1_seed, t1_public, "ab");
	ssh_buf_init(&out, 100, 1);
	CHECK_EQ(ssh_key_put_private(&key, 1, &out), EMSGSIZE);
	CHECK_EQ(out.size, 0);
	ssh_buf_free(&out);

	/* A comment that does not end with a NUL inside the array is refused. */
	make_key(&key, t1_seed, t1_public, "");
	memset(key.comment, 'a', sizeof(key.comment));
	ssh_buf_init(&out, SSH_KEY_FILE_MAX, 1);
	CHECK_EQ(ssh_key_put_private(&key, 1, &out), EINVAL);
	CHECK_EQ(out.size, 0);
	ssh_buf_free(&out);

	/* The longest comment (255 bytes) writes and reads back. */
	make_key(&key, t1_seed, t1_public, "");
	memset(key.comment, 'm', SSH_KEY_COMMENT_MAX - 1U);
	key.comment[SSH_KEY_COMMENT_MAX - 1U] = '\0';
	ssh_buf_init(&out, SSH_KEY_FILE_MAX, 1);
	CHECK_EQ(ssh_key_put_private(&key, 1, &out), 0);
	CHECK_EQ(ssh_key_parse_private((const char *)out.data, out.size, &back), 0);
	CHECK_BYTES(&back, &key, sizeof(key));
	ssh_buf_free(&out);

	/* A generated key round trips. */
	CHECK_EQ(ssh_key_generate(&key, "generated"), 0);
	ssh_buf_init(&out, SSH_KEY_FILE_MAX, 1);
	CHECK_EQ(ssh_key_put_private(&key, 0x01020304U, &out), 0);
	CHECK_EQ(ssh_key_parse_private((const char *)out.data, out.size, &back), 0);
	CHECK_BYTES(&back, &key, sizeof(key));
	ssh_buf_free(&out);

	/* Wipes both keys. */
	ssh_key_clear(&key);
	ssh_key_clear(&back);
}

/* Checks the SHA256 fingerprint. */
static void
test_fingerprint(void)
{
	struct wire *wire;
	uint8_t digest[SSH_SHA256_SIZE];
	char expected[80];
	char text[SSH_KEY_FINGERPRINT_SIZE + 8U];
	char encoded[64];
	size_t index;
	size_t text_length;
	int error;

	/* Works on the blob the key module builds from the public key. */
	wire = &outer_wire;

	/* The text worked out in this test: "SHA256:" and the SHA-256 of the blob in base64 without '='. */
	build_blob(wire, "ssh-ed25519", t1_public, SSH_ED25519_PUBLIC);
	error = ssh_sha256(wire->data, wire->size, digest);
	CHECK_EQ(error, 0);
	test_base64(digest, sizeof(digest), encoded, 0);
	strcpy(expected, "SHA256:");
	strcat(expected, encoded);
	CHECK_EQ(strlen(expected), SSH_KEY_FINGERPRINT_SIZE - 1U);

	/* It is also the text worked out by hand. */
	CHECK(strcmp(expected, t1_fingerprint) == 0);

	/* The module writes the same text and a NUL. */
	memset(text, 0x7e, sizeof(text));
	error = ssh_key_fingerprint(t1_public, text, SSH_KEY_FINGERPRINT_SIZE);
	CHECK_EQ(error, 0);
	CHECK(strcmp(text, expected) == 0);
	CHECK_EQ(text[SSH_KEY_FINGERPRINT_SIZE - 1U], 0);
	CHECK_EQ(text[SSH_KEY_FINGERPRINT_SIZE], 0x7e);

	/* A bigger buffer is fine. */
	memset(text, 0x7e, sizeof(text));
	error = ssh_key_fingerprint(t1_public, text, sizeof(text));
	CHECK_EQ(error, 0);
	CHECK(strcmp(text, expected) == 0);

	/* Another key gives another text. */
	error = ssh_key_fingerprint(t2_public, text, sizeof(text));
	CHECK_EQ(error, 0);
	CHECK(strcmp(text, expected) != 0);
	CHECK(memcmp(text, "SHA256:", 7) == 0);
	CHECK_EQ(strlen(text), SSH_KEY_FINGERPRINT_SIZE - 1U);
	text_length = strlen(text);
	for (index = 7; index < text_length; index++)
		CHECK(text[index] != '=');

	/* A capacity below 51 is ENOBUFS and writes nothing. */
	memset(text, 0x7e, sizeof(text));
	for (index = 0; index < SSH_KEY_FINGERPRINT_SIZE; index++) {
		error = ssh_key_fingerprint(t1_public, text, index);
		CHECK_EQ(error, ENOBUFS);
	}

	/* The refused calls wrote nothing. */
	CHECK_EQ(text[0], 0x7e);
	CHECK_EQ(text[SSH_KEY_FINGERPRINT_SIZE - 2U], 0x7e);
}

/* Reads a private key file with random changes: one byte changed, or the text cut; no crash, and a key only when it is a consistent one. */
static void
test_fuzz(void)
{
	struct file_spec spec;
	struct ssh_key key;
	uint8_t derived[SSH_ED25519_PUBLIC];
	static char original[TEST_TEXT_MAX];
	static char changed[TEST_TEXT_MAX];
	unsigned long state;
	unsigned long iteration;
	unsigned long successes;
	unsigned long failures;
	unsigned long bad;
	size_t length;
	size_t position;
	size_t size;
	int mode;
	int error;
	int good;
	int different;
	int public_error;
	int zero;

	/* The file to change: the TEST 1 key with a comment. */
	spec_init(&spec);
	spec.comment = "fuzz";
	spec.comment_size = 4;
	length = build_text(&spec);
	memcpy(original, file_text, length);

	/* Starts the generator at a fixed state and counts nothing yet. */
	state = 0x9e3779b9UL;
	successes = 0;
	failures = 0;
	bad = 0;

	/* Tries the changes. */
	for (iteration = 0; iteration < TEST_FUZZ_COUNT; iteration++) {
		/* A small deterministic generator (xorshift on 32 bits). */
		state ^= (state << 13) & 0xffffffffUL;
		state ^= state >> 17;
		state ^= (state << 5) & 0xffffffffUL;

		/* Starts from the original file and picks the change. */
		memcpy(changed, original, length);
		size = length;
		mode = (int)(state & 3U);
		position = (size_t)((state >> 2) % length);

		/* Mode 0 and 2 change one byte to a value that differs; mode 1 cuts the text; mode 3 does both. */
		if (mode == 0 || mode == 2 || mode == 3)
			changed[position] = (char)(changed[position] ^ (char)(((state >> 11) & 0xffU) | 1U));
		if (mode == 1 || mode == 3)
			size = (size_t)((state >> 19) % (length + 1U));

		/* Parses the changed text into a key filled with a known byte. */
		memset(&key, 0x5a, sizeof(key));
		error = ssh_key_parse_private(changed, size, &key);

		/* The result is one of the values the header names; a failure leaves a zero key, a success a consistent one. */
		good = 1;
		if (error == 0) {
			successes++;
			if (key.private != 1)
				good = 0;
			public_error = ssh_ed25519_public(key.seed, derived);
			if (public_error != 0) {
				good = 0;
			} else {
				different = memcmp(derived, key.public_key, SSH_ED25519_PUBLIC);
				if (different != 0)
					good = 0;
			}
		} else {
			failures++;
			if (error != EINVAL && error != ENOTSUP)
				good = 0;
			zero = key_is_zero(&key);
			if (!zero)
				good = 0;
		}

		/* Counts an iteration that gave a wrong result. */
		if (!good)
			bad++;
	}

	/* No iteration gave a wrong result, and both outcomes happened (the test changes things that matter and things that do not). */
	CHECK_EQ(bad, 0);
	CHECK(successes > 0);
	CHECK(failures > 0);
	CHECK_EQ(successes + failures, TEST_FUZZ_COUNT);
}
