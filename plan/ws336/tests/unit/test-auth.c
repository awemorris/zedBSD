/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The test of ssh-auth.c.  Every message is built here with wire.h from the
 * layouts of RFC 4253 section 10 (service request and accept) and of RFC
 * 4252 sections 5 (request), 5.1 (failure), 5.4 (banner), 7 (publickey,
 * the data that is signed, PK_OK) and 8 (password and the change form),
 * and the module's output is compared with those bytes; the module's
 * parsers read bytes built apart from the module's writers.  The key is
 * the secret key and public key of RFC 8032 section 7.1, TEST 1, and the
 * other public key is TEST 2.  The validity of UTF-8 follows the byte
 * ranges of RFC 3629 section 4, written out here as hand-picked cases.
 * Parsers read from a heap copy of exactly the message size, so that the
 * address sanitizer sees any read past the end.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "wire.h"
#include "ssh-auth.h"

/* The number of random messages tried on each parser. */
#define TEST_FUZZ_COUNT		100000UL

/* The size of the text buffers of this test, in bytes. */
#define TEST_TEXT_MAX		8192U

/*
 * The test keys, as hexadecimal text.
 *
 * Values referenced: RFC 8032 section 7.1, TEST 1 (secret key and public
 * key) and TEST 2 (public key).
 */
static const char t1_seed_hex[] =
	"9d61b19deffd5a60ba844af492ec2cc4 4449c5697b326919703bac031cae7f60";
static const char t1_public_hex[] =
	"d75a980182b10ab7d54bfed3c964073a 0ee172f3daa62325af021a68f707511a";
static const char t2_public_hex[] =
	"3d4017c3e843895a92b70aa74d1b7ebc 9c982ccf2ec4968cc0cd55f12af4660c";

/* The key bytes read from the hexadecimal text above. */
static uint8_t t1_seed[SSH_ED25519_SEED];
static uint8_t t1_public[SSH_ED25519_PUBLIC];
static uint8_t t2_public[SSH_ED25519_PUBLIC];

/* The key of TEST 1 as a private key structure. */
static struct ssh_key t1_key;

/* A session identifier for the signed data: 32 bytes, 0x00 to 0x1f. */
static uint8_t session_id[SSH_SHA256_SIZE];

/* The message being built and the message that is expected, apart from the module; they are large, so they are static. */
static struct wire built;
static struct wire expected;
static struct wire scratch;

/* The copy of the last message given to a parser, whose size is exactly the message's, so that a read past the end is caught. */
static uint8_t *held;

/* The 65 and 257 byte texts the limits are tried with. */
static char text65[66];
static char text64[65];
static char text256[257];
static char text257[258];

/* The state of the random generator. */
static unsigned long random_state;

/* Returns the next value of a small deterministic generator (xorshift on 32 bits). */
static unsigned long
random_next(
	void)
{
	/* Advances the state. */
	random_state ^= (random_state << 13) & 0xffffffffUL;
	random_state ^= random_state >> 17;
	random_state ^= (random_state << 5) & 0xffffffffUL;

	/* Hands out the new state. */
	return random_state;
}

/* Makes a heap copy of exactly size bytes and keeps it as the held message (the previous one is freed). */
static const uint8_t *
hold(
	const uint8_t *data,
	size_t size)
{
	/* Frees the previous copy. */
	free(held);

	/* Copies the bytes into a block of exactly their size. */
	held = malloc(size > 0 ? size : 1U);
	if (held == NULL) {
		printf("FAIL: out of memory\n");
		exit(2);
	}

	if (size > 0)
		memcpy(held, data, size);

	/* Hands out the copy. */
	return held;
}

/* Parses the first size bytes of data as a request from a heap copy of exactly that size. */
static int
parse_request_of(
	const uint8_t *data,
	size_t size,
	struct ssh_auth_request *request)
{
	const uint8_t *copy;

	/* Copies, then parses. */
	copy = hold(data, size);
	return ssh_auth_parse_request(copy, size, request);
}

/* Parses a built message as a request. */
static int
parse_request_wire(
	const struct wire *message,
	struct ssh_auth_request *request)
{
	/* Parses the bytes of the message. */
	return parse_request_of(message->data, message->size, request);
}

/* Reports whether the request structure holds only zero bytes. */
static int
request_is_zero(
	const struct ssh_auth_request *request)
{
	static struct ssh_auth_request zero;

	/* Compares the whole structure, padding included, with zeros. */
	return memcmp(request, &zero, sizeof(zero)) == 0;
}

/* Checks that a module-built buffer holds exactly the bytes of an expected message. */
#define CHECK_BUFFER(buffer, message) \
	do { \
		CHECK_EQ(ssh_buf_error(&(buffer)), 0); \
		CHECK_EQ((buffer).size, (message)->size); \
		if ((buffer).size == (message)->size) \
			CHECK_BYTES((buffer).data, (message)->data, (message)->size); \
	} while (0)

/* Starts the message with the first four fields of a USERAUTH_REQUEST (RFC 4252 5). */
static void
wire_request_head(
	struct wire *message,
	const char *user,
	const char *service,
	const char *method)
{
	/* The message number, then the three names. */
	wire_reset(message);
	wire_u8(message, 50);
	wire_text(message, user);
	wire_text(message, service);
	wire_text(message, method);
}

/* Appends a public key blob as a string (RFC 4253 6.6 and RFC 8709 4). */
static void
wire_key_blob(
	struct wire *message,
	const uint8_t *key)
{
	/* The blob is 4 + 11 + 4 + 32 bytes long. */
	wire_u32(message, 51);
	wire_text(message, "ssh-ed25519");
	wire_string(message, key, 32);
}

/* Appends the publickey fields up to the blob (RFC 4252 7) with has_signature as the byte given. */
static void
wire_publickey_fields(
	struct wire *message,
	unsigned has_signature,
	const char *algorithm,
	const uint8_t *key)
{
	/* The flag, the algorithm and the blob. */
	wire_bool(message, has_signature);
	wire_text(message, algorithm);
	wire_key_blob(message, key);
}

/* Appends a signature blob as a string (RFC 8709 6): "ssh-ed25519" and 64 bytes. */
static void
wire_signature(
	struct wire *message,
	uint8_t fill)
{
	uint8_t bytes[64];

	/* Makes 64 bytes of the fill value. */
	memset(bytes, fill, sizeof(bytes));

	/* The signature string holds the algorithm string and the 64-byte string. */
	wire_u32(message, 4 + 11 + 4 + 64);
	wire_text(message, "ssh-ed25519");
	wire_string(message, bytes, sizeof(bytes));
}

/* Returns the password request of RFC 4252 8 for user and password, with change as the byte given and the new password when not NULL. */
static void
wire_password_request(
	struct wire *message,
	const char *user,
	unsigned change,
	const char *password,
	const char *new_password)
{
	/* The head and the flag. */
	wire_request_head(message, user, "ssh-connection", "password");
	wire_bool(message, change);

	/* The password, and the new one in the change form. */
	wire_text(message, password);
	if (new_password != NULL)
		wire_text(message, new_password);
}

/* Reads the hexadecimal key texts into bytes and builds the key and session id. */
static void
setup_keys(
	void)
{
	unsigned index;

	/* Reads the hexadecimal text. */
	CHECK_EQ(wire_hex(t1_seed_hex, t1_seed, sizeof(t1_seed)), 32);
	CHECK_EQ(wire_hex(t1_public_hex, t1_public, sizeof(t1_public)), 32);
	CHECK_EQ(wire_hex(t2_public_hex, t2_public, sizeof(t2_public)), 32);

	/* Builds the private key structure of TEST 1. */
	memset(&t1_key, 0, sizeof(t1_key));
	memcpy(t1_key.seed, t1_seed, sizeof(t1_seed));
	memcpy(t1_key.public_key, t1_public, sizeof(t1_public));
	t1_key.private = 1;

	/* The module's own derivation agrees with the RFC's public key. */
	{
		uint8_t derived[SSH_ED25519_PUBLIC];

		CHECK_EQ(ssh_ed25519_public(t1_seed, derived), 0);
		CHECK_BYTES(derived, t1_public, sizeof(derived));
	}

	/* Makes the session identifier. */
	for (index = 0; index < SSH_SHA256_SIZE; index++)
		session_id[index] = (uint8_t)index;

	/* Makes the long texts. */
	memset(text64, 'a', 64);
	text64[64] = '\0';
	memset(text65, 'a', 65);
	text65[65] = '\0';
	memset(text256, 'p', 256);
	text256[256] = '\0';
	memset(text257, 'p', 257);
	text257[257] = '\0';
}

/* Tests the writers of the service messages against bytes built by wire.h. */
static void
test_service_put(
	void)
{
	struct ssh_buf out;

	/* SSH_MSG_SERVICE_REQUEST is number 5 (RFC 4250 4.1.2): the number and the name as a string. */
	wire_reset(&expected);
	wire_u8(&expected, 5);
	wire_text(&expected, "ssh-userauth");
	ssh_buf_init(&out, 1024, 0);
	ssh_auth_put_service_request("ssh-userauth", &out);
	CHECK_BUFFER(out, &expected);
	ssh_buf_free(&out);

	/* The same name is the constant the header gives. */
	CHECK(strcmp(SSH_SERVICE_USERAUTH, "ssh-userauth") == 0);
	CHECK(strcmp(SSH_SERVICE_CONNECTION, "ssh-connection") == 0);

	/* SSH_MSG_SERVICE_ACCEPT is number 6. */
	wire_reset(&expected);
	wire_u8(&expected, 6);
	wire_text(&expected, "ssh-connection");
	ssh_buf_init(&out, 1024, 0);
	ssh_auth_put_service_accept("ssh-connection", &out);
	CHECK_BUFFER(out, &expected);
	ssh_buf_free(&out);

	/* A buffer too small records its error. */
	ssh_buf_init(&out, 5, 0);
	ssh_auth_put_service_request("ssh-userauth", &out);
	CHECK_EQ(ssh_buf_error(&out), EMSGSIZE);
	ssh_buf_free(&out);
}

/* Tests the reader of the service messages. */
static void
test_service_parse(
	void)
{
	char service[SSH_NAME_MAX + 1];
	size_t length;
	int error;

	/* A request is read. */
	wire_reset(&built);
	wire_u8(&built, 5);
	wire_text(&built, "ssh-userauth");
	memset(service, 'x', sizeof(service));
	error = ssh_auth_parse_service(hold(built.data, built.size), built.size, service);
	CHECK_EQ(error, 0);
	CHECK(strcmp(service, "ssh-userauth") == 0);

	/* Every shorter prefix is malformed, and the name is left empty. */
	for (length = 0; length < built.size; length++) {
		memset(service, 'x', sizeof(service));
		error = ssh_auth_parse_service(hold(built.data, length), length, service);
		CHECK_EQ(error, EINVAL);
		CHECK_EQ(service[0], 0);
	}

	/* An accept is read the same way. */
	wire_reset(&built);
	wire_u8(&built, 6);
	wire_text(&built, "ssh-connection");
	error = ssh_auth_parse_service(hold(built.data, built.size), built.size, service);
	CHECK_EQ(error, 0);
	CHECK(strcmp(service, "ssh-connection") == 0);

	/* Another message number is refused: 4, 7 (SSH_MSG_EXT_INFO) and 50. */
	built.data[0] = 4;
	CHECK_EQ(ssh_auth_parse_service(hold(built.data, built.size), built.size, service), EINVAL);
	built.data[0] = 7;
	CHECK_EQ(ssh_auth_parse_service(hold(built.data, built.size), built.size, service), EINVAL);
	built.data[0] = 50;
	CHECK_EQ(ssh_auth_parse_service(hold(built.data, built.size), built.size, service), EINVAL);

	/* A byte after the name is refused. */
	wire_reset(&built);
	wire_u8(&built, 5);
	wire_text(&built, "ssh-userauth");
	wire_u8(&built, 0);
	CHECK_EQ(ssh_auth_parse_service(hold(built.data, built.size), built.size, service), EINVAL);

	/* A name with a NUL inside is refused. */
	wire_reset(&built);
	wire_u8(&built, 5);
	wire_string(&built, "ssh\0auth", 8);
	CHECK_EQ(ssh_auth_parse_service(hold(built.data, built.size), built.size, service), EINVAL);

	/* A name of 64 bytes (RFC 4251 6) is the longest accepted; 65 is too long. */
	wire_reset(&built);
	wire_u8(&built, 5);
	wire_text(&built, text64);
	CHECK_EQ(ssh_auth_parse_service(hold(built.data, built.size), built.size, service), 0);
	CHECK(strcmp(service, text64) == 0);
	wire_reset(&built);
	wire_u8(&built, 5);
	wire_text(&built, text65);
	CHECK_EQ(ssh_auth_parse_service(hold(built.data, built.size), built.size, service), EMSGSIZE);
	CHECK_EQ(service[0], 0);

	/* An empty name is a name of length 0, which is read as empty. */
	wire_reset(&built);
	wire_u8(&built, 5);
	wire_text(&built, "");
	CHECK_EQ(ssh_auth_parse_service(hold(built.data, built.size), built.size, service), 0);
	CHECK_EQ(service[0], 0);

	/* A length that runs past the end is refused. */
	wire_reset(&built);
	wire_u8(&built, 5);
	wire_u32(&built, 100);
	wire_bytes(&built, "abc", 3);
	CHECK_EQ(ssh_auth_parse_service(hold(built.data, built.size), built.size, service), EINVAL);

	/* A missing output is refused. */
	CHECK_EQ(ssh_auth_parse_service(built.data, built.size, NULL), EINVAL);
}

/* Tests the writers of the requests against bytes built by wire.h. */
static void
test_request_put(
	void)
{
	struct ssh_buf out;
	struct ssh_buf secret;

	/* A "none" request (RFC 4252 5.2). */
	wire_request_head(&expected, "alice", "ssh-connection", "none");
	ssh_buf_init(&out, 1024, 0);
	ssh_auth_put_none("alice", &out);
	CHECK_BUFFER(out, &expected);
	ssh_buf_free(&out);

	/* A publickey query: has_signature FALSE, the algorithm, the blob (RFC 4252 7). */
	wire_request_head(&expected, "alice", "ssh-connection", "publickey");
	wire_publickey_fields(&expected, 0, "ssh-ed25519", t1_public);
	ssh_buf_init(&out, 1024, 0);
	ssh_auth_put_publickey_query("alice", t1_public, &out);
	CHECK_BUFFER(out, &expected);
	ssh_buf_free(&out);

	/* A password request: the flag FALSE, then the password (RFC 4252 8). */
	wire_password_request(&expected, "alice", 0, "hunter2", NULL);
	ssh_buf_init(&secret, 1024, 1);
	ssh_auth_put_password("alice", "hunter2", &secret);
	CHECK_BUFFER(secret, &expected);
	ssh_buf_free(&secret);

	/* The failure message: the list and FALSE (RFC 4252 5.1). */
	wire_reset(&expected);
	wire_u8(&expected, 51);
	wire_text(&expected, "publickey,password");
	wire_bool(&expected, 0);
	ssh_buf_init(&out, 1024, 0);
	ssh_auth_put_failure("publickey,password", &out);
	CHECK_BUFFER(out, &expected);
	ssh_buf_free(&out);

	/* A failure with an empty list. */
	wire_reset(&expected);
	wire_u8(&expected, 51);
	wire_text(&expected, "");
	wire_bool(&expected, 0);
	ssh_buf_init(&out, 1024, 0);
	ssh_auth_put_failure("", &out);
	CHECK_BUFFER(out, &expected);
	ssh_buf_free(&out);

	/* The success message is the number 52 alone. */
	wire_reset(&expected);
	wire_u8(&expected, 52);
	ssh_buf_init(&out, 1024, 0);
	ssh_auth_put_success(&out);
	CHECK_BUFFER(out, &expected);
	ssh_buf_free(&out);

	/* PK_OK: number 60, the algorithm string and the blob string (RFC 4252 7). */
	wire_reset(&expected);
	wire_u8(&expected, 60);
	wire_text(&expected, "ssh-ed25519");
	wire_key_blob(&expected, t1_public);
	ssh_buf_init(&out, 1024, 0);
	ssh_auth_put_pk_ok(t1_public, &out);
	CHECK_BUFFER(out, &expected);
	ssh_buf_free(&out);

	/* The numbers of the header are the numbers of RFC 4252 6. */
	CHECK_EQ(SSH_MSG_USERAUTH_REQUEST, 50);
	CHECK_EQ(SSH_MSG_USERAUTH_FAILURE, 51);
	CHECK_EQ(SSH_MSG_USERAUTH_SUCCESS, 52);
	CHECK_EQ(SSH_MSG_USERAUTH_BANNER, 53);
	CHECK_EQ(SSH_MSG_USERAUTH_PK_OK, 60);

	/* A buffer too small records its error. */
	ssh_buf_init(&out, 20, 0);
	ssh_auth_put_none("alice", &out);
	CHECK_EQ(ssh_buf_error(&out), EMSGSIZE);
	ssh_buf_free(&out);
	ssh_buf_init(&out, 10, 0);
	ssh_auth_put_pk_ok(t1_public, &out);
	CHECK_EQ(ssh_buf_error(&out), EMSGSIZE);
	ssh_buf_free(&out);

	/* A missing password or user records EINVAL. */
	ssh_buf_init(&out, 1024, 0);
	ssh_auth_put_password("alice", NULL, &out);
	CHECK_EQ(ssh_buf_error(&out), EINVAL);
	ssh_buf_free(&out);
}

/* Tests the signed request: its layout, the signed data, and the check by ssh_key_verify. */
static void
test_signed(
	void)
{
	struct ssh_buf out;
	struct ssh_buf data;
	struct ssh_auth_request request;
	uint8_t other_id[SSH_SHA256_SIZE];
	uint8_t *copy;
	size_t head_size;
	size_t bit;
	int error;

	/* Builds the expected head of the signed request: has_signature TRUE and the blob (RFC 4252 7). */
	wire_request_head(&expected, "alice", "ssh-connection", "publickey");
	wire_publickey_fields(&expected, 1, "ssh-ed25519", t1_public);
	head_size = expected.size;

	/* Builds the signed request with the module. */
	ssh_buf_init(&out, 4096, 0);
	error = ssh_auth_put_publickey_signed("alice", &t1_key, session_id, &out);
	CHECK_EQ(error, 0);
	CHECK_EQ(out.size, head_size + 4 + 83);
	CHECK_BYTES(out.data, expected.data, head_size);

	/* The signature string that follows holds an 83 byte blob: "ssh-ed25519" and 64 bytes. */
	CHECK_EQ(ssh_load_be32(out.data + head_size), 83);
	CHECK_EQ(ssh_load_be32(out.data + head_size + 4), 11);
	CHECK_BYTES(out.data + head_size + 8, "ssh-ed25519", 11);
	CHECK_EQ(ssh_load_be32(out.data + head_size + 19), 64);

	/* A server reads it: the key, the flag, and the sizes. */
	error = parse_request_of(out.data, out.size, &request);
	CHECK_EQ(error, 0);
	CHECK_EQ(request.method, SSH_AUTH_PUBLICKEY);
	CHECK_EQ(request.supported, 1);
	CHECK_EQ(request.has_signature, 1);
	CHECK_BYTES(request.public_key, t1_public, 32);
	CHECK_EQ(request.signed_size, head_size);
	CHECK_EQ(request.signature_size, 83);
	CHECK(request.signature == held + head_size + 4);
	CHECK(strcmp(request.user, "alice") == 0);
	CHECK(strcmp(request.service, "ssh-connection") == 0);
	CHECK(strcmp(request.algorithm, "ssh-ed25519") == 0);
	CHECK_EQ(request.key_blob_size, 51);
	CHECK(request.key_blob == held + head_size - 51);

	/* The signed data is the session id string and the head of the payload (RFC 4252 7). */
	wire_reset(&scratch);
	wire_string(&scratch, session_id, 32);
	wire_bytes(&scratch, expected.data, head_size);
	ssh_buf_init(&data, 4096, 0);
	ssh_auth_put_signed_data(session_id, held, request.signed_size, &data);
	CHECK_BUFFER(data, &scratch);

	/* The signature verifies over exactly those bytes, with the public key the request names. */
	error = ssh_key_verify(request.public_key, scratch.data, scratch.size, request.signature, request.signature_size);
	CHECK_EQ(error, 0);
	ssh_buf_free(&data);

	/* A session id that differs in one bit, in each bit position, does not verify. */
	for (bit = 0; bit < 256; bit++) {
		memcpy(other_id, session_id, sizeof(other_id));
		other_id[bit / 8] = (uint8_t)(other_id[bit / 8] ^ (1U << (bit % 8)));
		ssh_buf_init(&data, 4096, 0);
		ssh_auth_put_signed_data(other_id, held, request.signed_size, &data);
		error = ssh_key_verify(request.public_key, data.data, data.size, request.signature, request.signature_size);
		CHECK_EQ(error, EBADMSG);
		ssh_buf_free(&data);
	}

	/* A request whose user differs (one byte changed) does not verify with the same signature. */
	copy = malloc(out.size);
	if (copy == NULL)
		exit(2);
	memcpy(copy, out.data, out.size);
	copy[5] = (uint8_t)'b';
	error = ssh_auth_parse_request(copy, out.size, &request);
	CHECK_EQ(error, 0);
	CHECK(strcmp(request.user, "blice") == 0);
	ssh_buf_init(&data, 4096, 0);
	ssh_auth_put_signed_data(session_id, copy, request.signed_size, &data);
	error = ssh_key_verify(request.public_key, data.data, data.size, request.signature, request.signature_size);
	CHECK_EQ(error, EBADMSG);
	ssh_buf_free(&data);
	free(copy);

	/* One flipped bit anywhere in the signed part (service, method, key) is also caught. */
	for (bit = 0; bit < head_size * 8; bit++) {
		copy = malloc(out.size);
		if (copy == NULL)
			exit(2);
		memcpy(copy, out.data, out.size);
		copy[bit / 8] = (uint8_t)(copy[bit / 8] ^ (1U << (bit % 8)));
		ssh_buf_init(&data, 4096, 0);
		ssh_auth_put_signed_data(session_id, copy, head_size, &data);
		error = ssh_key_verify(t1_public, data.data, data.size, out.data + head_size + 4, 83);
		CHECK_EQ(error, EBADMSG);
		ssh_buf_free(&data);
		free(copy);
	}

	/* A second request after the first appends after it, and signs only its own bytes. */
	ssh_buf_free(&out);
	ssh_buf_init(&out, 4096, 0);
	ssh_buf_put_bytes(&out, "prefix", 6);
	error = ssh_auth_put_publickey_signed("alice", &t1_key, session_id, &out);
	CHECK_EQ(error, 0);
	CHECK_EQ(out.size, 6 + head_size + 4 + 83);
	CHECK_BYTES(out.data, "prefix", 6);
	CHECK_BYTES(out.data + 6, expected.data, head_size);
	wire_reset(&scratch);
	wire_string(&scratch, session_id, 32);
	wire_bytes(&scratch, expected.data, head_size);
	error = ssh_key_verify(t1_public, scratch.data, scratch.size, out.data + 6 + head_size + 4, 83);
	CHECK_EQ(error, 0);
	ssh_buf_free(&out);

	/* A key without a seed cannot sign: EINVAL, and nothing is appended. */
	{
		struct ssh_key public_only;

		memset(&public_only, 0, sizeof(public_only));
		memcpy(public_only.public_key, t1_public, 32);
		ssh_buf_init(&out, 4096, 0);
		error = ssh_auth_put_publickey_signed("alice", &public_only, session_id, &out);
		CHECK_EQ(error, EINVAL);
		CHECK_EQ(out.size, 0);
		ssh_buf_free(&out);
	}

	/* Missing arguments are EINVAL. */
	ssh_buf_init(&out, 4096, 0);
	CHECK_EQ(ssh_auth_put_publickey_signed(NULL, &t1_key, session_id, &out), EINVAL);
	CHECK_EQ(ssh_auth_put_publickey_signed("alice", NULL, session_id, &out), EINVAL);
	CHECK_EQ(ssh_auth_put_publickey_signed("alice", &t1_key, NULL, &out), EINVAL);
	CHECK_EQ(out.size, 0);
	ssh_buf_free(&out);

	/* A buffer too small for the request gives its error. */
	ssh_buf_init(&out, 100, 0);
	error = ssh_auth_put_publickey_signed("alice", &t1_key, session_id, &out);
	CHECK_EQ(error, EMSGSIZE);
	ssh_buf_free(&out);
	ssh_buf_init(&out, head_size + 10, 0);
	error = ssh_auth_put_publickey_signed("alice", &t1_key, session_id, &out);
	CHECK_EQ(error, EMSGSIZE);
	ssh_buf_free(&out);

	/* put_signed_data with a missing id or missing bytes records EINVAL. */
	ssh_buf_init(&data, 4096, 0);
	ssh_auth_put_signed_data(NULL, expected.data, 10, &data);
	CHECK_EQ(ssh_buf_error(&data), EINVAL);
	ssh_buf_free(&data);
	ssh_buf_init(&data, 4096, 0);
	ssh_auth_put_signed_data(session_id, NULL, 10, &data);
	CHECK_EQ(ssh_buf_error(&data), EINVAL);
	ssh_buf_free(&data);

	/* With no payload bytes at all, the data is the session id string alone. */
	ssh_buf_init(&data, 4096, 0);
	ssh_auth_put_signed_data(session_id, NULL, 0, &data);
	wire_reset(&scratch);
	wire_string(&scratch, session_id, 32);
	CHECK_BUFFER(data, &scratch);
	ssh_buf_free(&data);
}

/* Tests parse_request on the requests of the methods. */
static void
test_request_parse(
	void)
{
	struct ssh_auth_request request;
	int error;

	/* "none": the user, the service, and nothing more. */
	wire_request_head(&built, "alice", "ssh-connection", "none");
	memset(&request, 0xaa, sizeof(request));
	error = parse_request_wire(&built, &request);
	CHECK_EQ(error, 0);
	CHECK_EQ(request.method, SSH_AUTH_NONE);
	CHECK(strcmp(request.user, "alice") == 0);
	CHECK(strcmp(request.service, "ssh-connection") == 0);
	CHECK_EQ(request.supported, 0);
	CHECK_EQ(request.change, 0);
	CHECK_EQ(request.has_signature, 0);
	CHECK(request.key_blob == NULL);
	CHECK(request.signature == NULL);
	CHECK_EQ(request.password[0], 0);

	/* A byte after "none" is refused, and the request is cleared. */
	wire_u8(&built, 0);
	memset(&request, 0xaa, sizeof(request));
	CHECK_EQ(parse_request_wire(&built, &request), EINVAL);
	CHECK(request_is_zero(&request));

	/* A publickey query without a signature. */
	wire_request_head(&built, "bob", "ssh-connection", "publickey");
	wire_publickey_fields(&built, 0, "ssh-ed25519", t1_public);
	error = parse_request_wire(&built, &request);
	CHECK_EQ(error, 0);
	CHECK_EQ(request.method, SSH_AUTH_PUBLICKEY);
	CHECK_EQ(request.supported, 1);
	CHECK_EQ(request.has_signature, 0);
	CHECK(request.signature == NULL);
	CHECK_EQ(request.signature_size, 0);
	CHECK_EQ(request.signed_size, 0);
	CHECK_BYTES(request.public_key, t1_public, 32);
	CHECK_EQ(request.key_blob_size, 51);
	CHECK(request.key_blob == held + built.size - 51);
	CHECK(strcmp(request.algorithm, "ssh-ed25519") == 0);

	/* A boolean other than 0 and 1 is TRUE (RFC 4251 5); with a signature the request is the signed form. */
	wire_request_head(&built, "bob", "ssh-connection", "publickey");
	wire_publickey_fields(&built, 2, "ssh-ed25519", t2_public);
	wire_signature(&built, 0x11);
	error = parse_request_wire(&built, &request);
	CHECK_EQ(error, 0);
	CHECK_EQ(request.has_signature, 1);
	CHECK_EQ(request.supported, 1);
	CHECK_BYTES(request.public_key, t2_public, 32);
	CHECK_EQ(request.signed_size, built.size - 4 - 83);
	CHECK_EQ(request.signature_size, 83);
	CHECK(request.signature == held + built.size - 83);

	/* Every shorter prefix of the signed request is malformed, and the request is cleared. */
	{
		size_t length;

		for (length = 0; length < built.size; length++) {
			memset(&request, 0xaa, sizeof(request));
			error = parse_request_of(built.data, length, &request);
			CHECK_EQ(error, EINVAL);
			CHECK(request_is_zero(&request));
		}
	}

	/* A byte after the signature is refused. */
	wire_u8(&built, 0);
	CHECK_EQ(parse_request_wire(&built, &request), EINVAL);

	/* The "ssh-rsa" algorithm is read but unsupported. */
	wire_request_head(&built, "bob", "ssh-connection", "publickey");
	wire_publickey_fields(&built, 0, "ssh-rsa", t1_public);
	error = parse_request_wire(&built, &request);
	CHECK_EQ(error, 0);
	CHECK_EQ(request.supported, 0);
	CHECK(strcmp(request.algorithm, "ssh-rsa") == 0);
	CHECK_EQ(request.key_blob_size, 51);

	/* An algorithm of a longer name than ssh-ed25519 and a prefix of it are both unsupported. */
	wire_request_head(&built, "bob", "ssh-connection", "publickey");
	wire_publickey_fields(&built, 0, "ssh-ed25519-cert-v01@openssh.com", t1_public);
	CHECK_EQ(parse_request_wire(&built, &request), 0);
	CHECK_EQ(request.supported, 0);
	wire_request_head(&built, "bob", "ssh-connection", "publickey");
	wire_publickey_fields(&built, 0, "ssh-ed2551", t1_public);
	CHECK_EQ(parse_request_wire(&built, &request), 0);
	CHECK_EQ(request.supported, 0);

	/* A blob that names ssh-rsa under the algorithm ssh-ed25519 is unsupported. */
	wire_request_head(&built, "bob", "ssh-connection", "publickey");
	wire_bool(&built, 0);
	wire_text(&built, "ssh-ed25519");
	wire_u32(&built, 4 + 7 + 4 + 32);
	wire_text(&built, "ssh-rsa");
	wire_string(&built, t1_public, 32);
	CHECK_EQ(parse_request_wire(&built, &request), 0);
	CHECK_EQ(request.supported, 0);
	CHECK_EQ(request.key_blob_size, 4 + 7 + 4 + 32);

	/* A blob of ssh-ed25519 under the algorithm ssh-rsa is unsupported (the types differ). */
	wire_request_head(&built, "bob", "ssh-connection", "publickey");
	wire_publickey_fields(&built, 0, "ssh-rsa", t2_public);
	CHECK_EQ(parse_request_wire(&built, &request), 0);
	CHECK_EQ(request.supported, 0);

	/* A blob whose key is 31 bytes, or has a byte after it, is unsupported but the request is well formed. */
	wire_request_head(&built, "bob", "ssh-connection", "publickey");
	wire_bool(&built, 0);
	wire_text(&built, "ssh-ed25519");
	wire_u32(&built, 4 + 11 + 4 + 31);
	wire_text(&built, "ssh-ed25519");
	wire_string(&built, t1_public, 31);
	CHECK_EQ(parse_request_wire(&built, &request), 0);
	CHECK_EQ(request.supported, 0);
	wire_request_head(&built, "bob", "ssh-connection", "publickey");
	wire_bool(&built, 0);
	wire_text(&built, "ssh-ed25519");
	wire_u32(&built, 4 + 11 + 4 + 32 + 1);
	wire_text(&built, "ssh-ed25519");
	wire_string(&built, t1_public, 32);
	wire_u8(&built, 0);
	CHECK_EQ(parse_request_wire(&built, &request), 0);
	CHECK_EQ(request.supported, 0);

	/* An empty blob is unsupported too. */
	wire_request_head(&built, "bob", "ssh-connection", "publickey");
	wire_bool(&built, 0);
	wire_text(&built, "ssh-ed25519");
	wire_text(&built, "");
	CHECK_EQ(parse_request_wire(&built, &request), 0);
	CHECK_EQ(request.supported, 0);
	CHECK_EQ(request.key_blob_size, 0);

	/* An algorithm name of 65 bytes, and a NUL inside the algorithm, are malformed. */
	wire_request_head(&built, "bob", "ssh-connection", "publickey");
	wire_publickey_fields(&built, 0, text65, t1_public);
	CHECK_EQ(parse_request_wire(&built, &request), EINVAL);
	wire_request_head(&built, "bob", "ssh-connection", "publickey");
	wire_bool(&built, 0);
	wire_string(&built, "ssh-ed\0" "25519", 12);
	wire_key_blob(&built, t1_public);
	CHECK_EQ(parse_request_wire(&built, &request), EINVAL);

	/* A password request: FALSE and the password. */
	wire_password_request(&built, "carol", 0, "s3cret pass", NULL);
	error = parse_request_wire(&built, &request);
	CHECK_EQ(error, 0);
	CHECK_EQ(request.method, SSH_AUTH_PASSWORD);
	CHECK_EQ(request.change, 0);
	CHECK(strcmp(request.password, "s3cret pass") == 0);
	CHECK(strcmp(request.user, "carol") == 0);

	/* The request is cleared, the password wiped, by ssh_auth_request_clear. */
	ssh_auth_request_clear(&request);
	CHECK(request_is_zero(&request));

	/* An empty password is read. */
	wire_password_request(&built, "carol", 0, "", NULL);
	CHECK_EQ(parse_request_wire(&built, &request), 0);
	CHECK_EQ(request.password[0], 0);

	/* A password of 256 bytes is the longest; 257 is refused whole. */
	wire_password_request(&built, "carol", 0, text256, NULL);
	CHECK_EQ(parse_request_wire(&built, &request), 0);
	CHECK(strcmp(request.password, text256) == 0);
	wire_password_request(&built, "carol", 0, text257, NULL);
	memset(&request, 0xaa, sizeof(request));
	CHECK_EQ(parse_request_wire(&built, &request), EINVAL);
	CHECK(request_is_zero(&request));

	/* A password with a NUL inside is refused. */
	wire_request_head(&built, "carol", "ssh-connection", "password");
	wire_bool(&built, 0);
	wire_string(&built, "ab\0cd", 5);
	CHECK_EQ(parse_request_wire(&built, &request), EINVAL);

	/* The change form (RFC 4252 8): the old and the new password; the request says so. */
	wire_password_request(&built, "carol", 1, "old", "new");
	error = parse_request_wire(&built, &request);
	CHECK_EQ(error, 0);
	CHECK_EQ(request.method, SSH_AUTH_PASSWORD);
	CHECK_EQ(request.change, 1);
	CHECK(strcmp(request.password, "old") == 0);

	/* A flag byte of 2 is TRUE. */
	wire_password_request(&built, "carol", 2, "old", "new");
	CHECK_EQ(parse_request_wire(&built, &request), 0);
	CHECK_EQ(request.change, 1);

	/* The change form without the new password, or with a too long one, is malformed. */
	wire_password_request(&built, "carol", 1, "old", NULL);
	CHECK_EQ(parse_request_wire(&built, &request), EINVAL);
	CHECK(request_is_zero(&request));
	wire_password_request(&built, "carol", 1, "old", text257);
	CHECK_EQ(parse_request_wire(&built, &request), EINVAL);
	wire_password_request(&built, "carol", 1, "old", text256);
	CHECK_EQ(parse_request_wire(&built, &request), 0);
	wire_password_request(&built, "carol", 1, text257, "new");
	CHECK_EQ(parse_request_wire(&built, &request), EINVAL);

	/* A new password with a NUL inside is malformed. */
	wire_request_head(&built, "carol", "ssh-connection", "password");
	wire_bool(&built, 1);
	wire_text(&built, "old");
	wire_string(&built, "n\0w", 3);
	CHECK_EQ(parse_request_wire(&built, &request), EINVAL);

	/* A byte after the password, in either form, is malformed. */
	wire_password_request(&built, "carol", 0, "old", "new");
	CHECK_EQ(parse_request_wire(&built, &request), EINVAL);
	wire_password_request(&built, "carol", 1, "old", "new");
	wire_u8(&built, 0);
	CHECK_EQ(parse_request_wire(&built, &request), EINVAL);

	/* Every shorter prefix of a change request is malformed. */
	wire_password_request(&built, "carol", 1, "old", "new");
	{
		size_t length;

		for (length = 0; length < built.size; length++)
			CHECK_EQ(parse_request_of(built.data, length, &request), EINVAL);
	}
}

/* Tests the names, the unknown methods and the message number of a request. */
static void
test_request_names(
	void)
{
	struct ssh_auth_request request;
	int error;

	/* A user of 64 bytes is the longest; 65 is malformed. */
	wire_request_head(&built, text64, "ssh-connection", "none");
	CHECK_EQ(parse_request_wire(&built, &request), 0);
	CHECK(strcmp(request.user, text64) == 0);
	wire_request_head(&built, text65, "ssh-connection", "none");
	memset(&request, 0xaa, sizeof(request));
	CHECK_EQ(parse_request_wire(&built, &request), EINVAL);
	CHECK(request_is_zero(&request));

	/* A NUL inside the user is malformed. */
	wire_reset(&built);
	wire_u8(&built, 50);
	wire_string(&built, "al\0ce", 5);
	wire_text(&built, "ssh-connection");
	wire_text(&built, "none");
	CHECK_EQ(parse_request_wire(&built, &request), EINVAL);

	/* An empty user is read (the program decides what to do with it). */
	wire_request_head(&built, "", "ssh-connection", "none");
	CHECK_EQ(parse_request_wire(&built, &request), 0);
	CHECK_EQ(request.user[0], 0);

	/* A service of 64 bytes is read, 65 and one with a NUL are malformed, another service name is read as it is. */
	wire_request_head(&built, "alice", text64, "none");
	CHECK_EQ(parse_request_wire(&built, &request), 0);
	CHECK(strcmp(request.service, text64) == 0);
	wire_request_head(&built, "alice", text65, "none");
	memset(&request, 0xaa, sizeof(request));
	CHECK_EQ(parse_request_wire(&built, &request), EINVAL);
	CHECK(request_is_zero(&request));
	wire_reset(&built);
	wire_u8(&built, 50);
	wire_text(&built, "alice");
	wire_string(&built, "ssh\0connection", 14);
	wire_text(&built, "none");
	CHECK_EQ(parse_request_wire(&built, &request), EINVAL);
	wire_request_head(&built, "alice", "ssh-userauth", "none");
	CHECK_EQ(parse_request_wire(&built, &request), 0);
	CHECK(strcmp(request.service, "ssh-userauth") == 0);

	/* An unknown method is not an error, and the rest of the payload is not read (any bytes may follow). */
	wire_request_head(&built, "alice", "ssh-connection", "hostbased");
	wire_text(&built, "ssh-rsa");
	wire_u32(&built, 0xffffffffU);
	error = parse_request_wire(&built, &request);
	CHECK_EQ(error, 0);
	CHECK_EQ(request.method, SSH_AUTH_OTHER);
	CHECK_EQ(request.supported, 0);
	CHECK(strcmp(request.user, "alice") == 0);
	wire_request_head(&built, "alice", "ssh-connection", "keyboard-interactive");
	CHECK_EQ(parse_request_wire(&built, &request), 0);
	CHECK_EQ(request.method, SSH_AUTH_OTHER);

	/* Method names that differ in case, with a NUL, or with a tail are not the known methods. */
	wire_request_head(&built, "alice", "ssh-connection", "Password");
	CHECK_EQ(parse_request_wire(&built, &request), 0);
	CHECK_EQ(request.method, SSH_AUTH_OTHER);
	wire_request_head(&built, "alice", "ssh-connection", "PUBLICKEY");
	CHECK_EQ(parse_request_wire(&built, &request), 0);
	CHECK_EQ(request.method, SSH_AUTH_OTHER);
	wire_request_head(&built, "alice", "ssh-connection", "nonex");
	CHECK_EQ(parse_request_wire(&built, &request), 0);
	CHECK_EQ(request.method, SSH_AUTH_OTHER);
	wire_request_head(&built, "alice", "ssh-connection", "non");
	CHECK_EQ(parse_request_wire(&built, &request), 0);
	CHECK_EQ(request.method, SSH_AUTH_OTHER);
	wire_request_head(&built, "alice", "ssh-connection", "nonf");
	CHECK_EQ(parse_request_wire(&built, &request), 0);
	CHECK_EQ(request.method, SSH_AUTH_OTHER);
	wire_request_head(&built, "alice", "ssh-connection", "xublickey");
	CHECK_EQ(parse_request_wire(&built, &request), 0);
	CHECK_EQ(request.method, SSH_AUTH_OTHER);
	wire_request_head(&built, "alice", "ssh-connection", "publickex");
	CHECK_EQ(parse_request_wire(&built, &request), 0);
	CHECK_EQ(request.method, SSH_AUTH_OTHER);
	wire_request_head(&built, "alice", "ssh-connection", "passwore");
	CHECK_EQ(parse_request_wire(&built, &request), 0);
	CHECK_EQ(request.method, SSH_AUTH_OTHER);
	wire_request_head(&built, "alice", "ssh-connection", "xassword");
	CHECK_EQ(parse_request_wire(&built, &request), 0);
	CHECK_EQ(request.method, SSH_AUTH_OTHER);
	wire_reset(&built);
	wire_u8(&built, 50);
	wire_text(&built, "alice");
	wire_text(&built, "ssh-connection");
	wire_string(&built, "none\0", 5);
	CHECK_EQ(parse_request_wire(&built, &request), 0);
	CHECK_EQ(request.method, SSH_AUTH_OTHER);
	wire_reset(&built);
	wire_u8(&built, 50);
	wire_text(&built, "alice");
	wire_text(&built, "ssh-connection");
	wire_text(&built, "");
	CHECK_EQ(parse_request_wire(&built, &request), 0);
	CHECK_EQ(request.method, SSH_AUTH_OTHER);

	/* A very long method name is still just unknown. */
	wire_request_head(&built, "alice", "ssh-connection", text257);
	CHECK_EQ(parse_request_wire(&built, &request), 0);
	CHECK_EQ(request.method, SSH_AUTH_OTHER);

	/* Another message number is refused: 0, 49, 51, 80. */
	wire_request_head(&built, "alice", "ssh-connection", "none");
	built.data[0] = 0;
	CHECK_EQ(parse_request_wire(&built, &request), EINVAL);
	built.data[0] = 49;
	CHECK_EQ(parse_request_wire(&built, &request), EINVAL);
	built.data[0] = 51;
	CHECK_EQ(parse_request_wire(&built, &request), EINVAL);
	built.data[0] = 80;
	CHECK_EQ(parse_request_wire(&built, &request), EINVAL);

	/* An empty payload, and a payload that ends in a field, are malformed. */
	memset(&request, 0xaa, sizeof(request));
	CHECK_EQ(parse_request_of(built.data, 0, &request), EINVAL);
	CHECK(request_is_zero(&request));
	wire_request_head(&built, "alice", "ssh-connection", "none");
	built.size -= 2;
	memset(&request, 0xaa, sizeof(request));
	CHECK_EQ(parse_request_wire(&built, &request), EINVAL);
	CHECK(request_is_zero(&request));
	wire_reset(&built);
	wire_u8(&built, 50);
	wire_u32(&built, 5);
	wire_bytes(&built, "ali", 3);
	CHECK_EQ(parse_request_wire(&built, &request), EINVAL);

	/* Missing arguments: no payload with a size, and no request structure. */
	CHECK_EQ(ssh_auth_parse_request(NULL, 5, &request), EINVAL);
	CHECK(request_is_zero(&request));
	CHECK_EQ(ssh_auth_parse_request(built.data, built.size, NULL), EINVAL);

	/* Clearing a NULL request is harmless. */
	ssh_auth_request_clear(NULL);
}

/* Tests the reader of USERAUTH_FAILURE. */
static void
test_failure_parse(
	void)
{
	char methods[64];
	int partial;
	size_t length;
	int error;

	/* A list of two methods and FALSE. */
	wire_reset(&built);
	wire_u8(&built, 51);
	wire_text(&built, "publickey,password");
	wire_bool(&built, 0);
	partial = 7;
	error = ssh_auth_parse_failure(hold(built.data, built.size), built.size, methods, sizeof(methods), &partial);
	CHECK_EQ(error, 0);
	CHECK(strcmp(methods, "publickey,password") == 0);
	CHECK_EQ(partial, 0);

	/* Every shorter prefix is malformed, and the outputs are emptied. */
	for (length = 0; length < built.size; length++) {
		memset(methods, 'x', sizeof(methods));
		partial = 7;
		error = ssh_auth_parse_failure(hold(built.data, length), length, methods, sizeof(methods), &partial);
		CHECK_EQ(error, EINVAL);
		CHECK_EQ(methods[0], 0);
		CHECK_EQ(partial, 0);
	}

	/* A byte after the flag is malformed. */
	wire_u8(&built, 0);
	CHECK_EQ(ssh_auth_parse_failure(hold(built.data, built.size), built.size, methods, sizeof(methods), &partial), EINVAL);

	/* A partial success of TRUE, as 1 and as another non-zero byte. */
	wire_reset(&built);
	wire_u8(&built, 51);
	wire_text(&built, "password");
	wire_bool(&built, 1);
	CHECK_EQ(ssh_auth_parse_failure(hold(built.data, built.size), built.size, methods, sizeof(methods), &partial), 0);
	CHECK_EQ(partial, 1);
	CHECK(strcmp(methods, "password") == 0);
	wire_reset(&built);
	wire_u8(&built, 51);
	wire_text(&built, "password");
	wire_bool(&built, 9);
	CHECK_EQ(ssh_auth_parse_failure(hold(built.data, built.size), built.size, methods, sizeof(methods), &partial), 0);
	CHECK_EQ(partial, 1);

	/* A NULL partial is allowed. */
	CHECK_EQ(ssh_auth_parse_failure(hold(built.data, built.size), built.size, methods, sizeof(methods), NULL), 0);

	/* An empty list is read as an empty string. */
	wire_reset(&built);
	wire_u8(&built, 51);
	wire_text(&built, "");
	wire_bool(&built, 0);
	memset(methods, 'x', sizeof(methods));
	CHECK_EQ(ssh_auth_parse_failure(hold(built.data, built.size), built.size, methods, sizeof(methods), &partial), 0);
	CHECK_EQ(methods[0], 0);

	/* The capacity: the list and its NUL must fit; exactly fitting is accepted, one less is EMSGSIZE. */
	wire_reset(&built);
	wire_u8(&built, 51);
	wire_text(&built, "publickey");
	wire_bool(&built, 0);
	CHECK_EQ(ssh_auth_parse_failure(hold(built.data, built.size), built.size, methods, 10, &partial), 0);
	CHECK(strcmp(methods, "publickey") == 0);
	memset(methods, 'x', sizeof(methods));
	CHECK_EQ(ssh_auth_parse_failure(hold(built.data, built.size), built.size, methods, 9, &partial), EMSGSIZE);
	CHECK_EQ(methods[0], 0);
	CHECK_EQ(ssh_auth_parse_failure(hold(built.data, built.size), built.size, methods, 0, &partial), EINVAL);
	CHECK_EQ(ssh_auth_parse_failure(hold(built.data, built.size), built.size, NULL, 10, &partial), EINVAL);

	/* A list that breaks the name-list rules (empty name, leading comma, trailing comma, a space, a high byte) is malformed. */
	wire_reset(&built);
	wire_u8(&built, 51);
	wire_text(&built, "a,,b");
	wire_bool(&built, 0);
	CHECK_EQ(ssh_auth_parse_failure(hold(built.data, built.size), built.size, methods, sizeof(methods), &partial), EINVAL);
	wire_reset(&built);
	wire_u8(&built, 51);
	wire_text(&built, ",a");
	wire_bool(&built, 0);
	CHECK_EQ(ssh_auth_parse_failure(hold(built.data, built.size), built.size, methods, sizeof(methods), &partial), EINVAL);
	wire_reset(&built);
	wire_u8(&built, 51);
	wire_text(&built, "a,");
	wire_bool(&built, 0);
	CHECK_EQ(ssh_auth_parse_failure(hold(built.data, built.size), built.size, methods, sizeof(methods), &partial), EINVAL);
	wire_reset(&built);
	wire_u8(&built, 51);
	wire_text(&built, "a b");
	wire_bool(&built, 0);
	CHECK_EQ(ssh_auth_parse_failure(hold(built.data, built.size), built.size, methods, sizeof(methods), &partial), EINVAL);
	wire_reset(&built);
	wire_u8(&built, 51);
	wire_text(&built, "a\xc3\xa9");
	wire_bool(&built, 0);
	CHECK_EQ(ssh_auth_parse_failure(hold(built.data, built.size), built.size, methods, sizeof(methods), &partial), EINVAL);

	/* Another message number is malformed. */
	wire_reset(&built);
	wire_u8(&built, 52);
	wire_text(&built, "password");
	wire_bool(&built, 0);
	CHECK_EQ(ssh_auth_parse_failure(hold(built.data, built.size), built.size, methods, sizeof(methods), &partial), EINVAL);
}

/* Tests the reader of USERAUTH_PK_OK. */
static void
test_pk_ok_parse(
	void)
{
	size_t length;
	size_t bit;
	int error;

	/* A reply for the key asked about is accepted. */
	wire_reset(&built);
	wire_u8(&built, 60);
	wire_text(&built, "ssh-ed25519");
	wire_key_blob(&built, t1_public);
	CHECK_EQ(ssh_auth_parse_pk_ok(hold(built.data, built.size), built.size, t1_public), 0);

	/* Every shorter prefix is malformed. */
	for (length = 0; length < built.size; length++)
		CHECK_EQ(ssh_auth_parse_pk_ok(hold(built.data, length), length, t1_public), EINVAL);

	/* A byte after the blob is malformed. */
	wire_u8(&built, 0);
	CHECK_EQ(ssh_auth_parse_pk_ok(hold(built.data, built.size), built.size, t1_public), EINVAL);

	/* A reply for another key is EPROTO. */
	wire_reset(&built);
	wire_u8(&built, 60);
	wire_text(&built, "ssh-ed25519");
	wire_key_blob(&built, t2_public);
	CHECK_EQ(ssh_auth_parse_pk_ok(hold(built.data, built.size), built.size, t1_public), EPROTO);

	/* A reply that differs from the key in any one bit is EPROTO. */
	for (bit = 0; bit < 256; bit++) {
		uint8_t changed[32];

		memcpy(changed, t1_public, 32);
		changed[bit / 8] = (uint8_t)(changed[bit / 8] ^ (1U << (bit % 8)));
		wire_reset(&built);
		wire_u8(&built, 60);
		wire_text(&built, "ssh-ed25519");
		wire_key_blob(&built, changed);
		error = ssh_auth_parse_pk_ok(hold(built.data, built.size), built.size, t1_public);
		CHECK_EQ(error, EPROTO);
	}

	/* A reply that names another algorithm is EPROTO, with the blob of either type. */
	wire_reset(&built);
	wire_u8(&built, 60);
	wire_text(&built, "ssh-rsa");
	wire_key_blob(&built, t1_public);
	CHECK_EQ(ssh_auth_parse_pk_ok(hold(built.data, built.size), built.size, t1_public), EPROTO);
	wire_reset(&built);
	wire_u8(&built, 60);
	wire_text(&built, "ssh-ed25519");
	wire_u32(&built, 4 + 7 + 4 + 32);
	wire_text(&built, "ssh-rsa");
	wire_string(&built, t1_public, 32);
	CHECK_EQ(ssh_auth_parse_pk_ok(hold(built.data, built.size), built.size, t1_public), EPROTO);

	/* A blob that is malformed is EINVAL. */
	wire_reset(&built);
	wire_u8(&built, 60);
	wire_text(&built, "ssh-ed25519");
	wire_u32(&built, 4 + 11 + 4 + 31);
	wire_text(&built, "ssh-ed25519");
	wire_string(&built, t1_public, 31);
	CHECK_EQ(ssh_auth_parse_pk_ok(hold(built.data, built.size), built.size, t1_public), EINVAL);

	/* Another message number is malformed: 61 and 51. */
	wire_reset(&built);
	wire_u8(&built, 61);
	wire_text(&built, "ssh-ed25519");
	wire_key_blob(&built, t1_public);
	CHECK_EQ(ssh_auth_parse_pk_ok(hold(built.data, built.size), built.size, t1_public), EINVAL);
	built.data[0] = 51;
	CHECK_EQ(ssh_auth_parse_pk_ok(hold(built.data, built.size), built.size, t1_public), EINVAL);

	/* A missing key is EINVAL. */
	CHECK_EQ(ssh_auth_parse_pk_ok(built.data, built.size, NULL), EINVAL);
}

/* Appends a message with a banner text and the language tag. */
static void
wire_banner(
	struct wire *message,
	const void *text,
	size_t size)
{
	/* The number, the message string and the language tag (RFC 4252 5.4). */
	wire_reset(message);
	wire_u8(message, 53);
	wire_string(message, text, size);
	wire_text(message, "en");
}

/* Reads a banner holding text and returns the filtered text in out (capacity 256). */
static int
banner_of(
	const void *text,
	size_t size,
	char *out)
{
	/* Builds the message and reads it from a heap copy. */
	wire_banner(&built, text, size);
	return ssh_auth_parse_banner(hold(built.data, built.size), built.size, out, 256);
}

/* Tests the banner filter. */
static void
test_banner(
	void)
{
	char out[256];
	static char big[TEST_TEXT_MAX];
	static char result[TEST_TEXT_MAX + 16];
	size_t length;
	int error;

	/* Plain text, LF and TAB are kept. */
	CHECK_EQ(banner_of("Hello\n\tWorld!", 13, out), 0);
	CHECK(strcmp(out, "Hello\n\tWorld!") == 0);

	/* An empty message is an empty text. */
	memset(out, 'x', sizeof(out));
	CHECK_EQ(banner_of("", 0, out), 0);
	CHECK_EQ(out[0], 0);

	/* ESC, CR, DEL, NUL, BEL, BS and the other C0 controls become '?'. */
	CHECK_EQ(banner_of("\x1b[31mred", 8, out), 0);
	CHECK(strcmp(out, "?[31mred") == 0);
	CHECK_EQ(banner_of("a\r\nb", 4, out), 0);
	CHECK(strcmp(out, "a?\nb") == 0);
	CHECK_EQ(banner_of("a\x7f" "b", 3, out), 0);
	CHECK(strcmp(out, "a?b") == 0);
	CHECK_EQ(banner_of("a\0b", 3, out), 0);
	CHECK(strcmp(out, "a?b") == 0);
	CHECK_EQ(banner_of("\x07\x08\x0b\x0c\x1f\x01", 6, out), 0);
	CHECK(strcmp(out, "??????") == 0);

	/* The boundaries of the C0 range: 0x1f is replaced, 0x20 (space) and 0x7e (~) are kept. */
	CHECK_EQ(banner_of("\x1f \x7e", 3, out), 0);
	CHECK(strcmp(out, "? ~") == 0);

	/* 0xc0 0x80 (an overlong NUL) is two bad bytes; C1 as a lone lead too. */
	CHECK_EQ(banner_of("\xc0\x80", 2, out), 0);
	CHECK(strcmp(out, "??") == 0);
	CHECK_EQ(banner_of("\xc1\xbf", 2, out), 0);
	CHECK(strcmp(out, "??") == 0);

	/* A continuation byte alone, and bytes that never occur in UTF-8 (F5 to FF), are replaced. */
	CHECK_EQ(banner_of("\x80\xbf\xf5\xff\xfe", 5, out), 0);
	CHECK(strcmp(out, "?????") == 0);

	/* Valid sequences are kept: U+00E9, U+20AC, U+1F600, U+0080 boundary neighbours U+00A0 and U+07FF, U+FFFF, U+10FFFF. */
	CHECK_EQ(banner_of("\xc3\xa9", 2, out), 0);
	CHECK(strcmp(out, "\xc3\xa9") == 0);
	CHECK_EQ(banner_of("\xe2\x82\xac", 3, out), 0);
	CHECK(strcmp(out, "\xe2\x82\xac") == 0);
	CHECK_EQ(banner_of("\xf0\x9f\x98\x80", 4, out), 0);
	CHECK(strcmp(out, "\xf0\x9f\x98\x80") == 0);
	CHECK_EQ(banner_of("\xc2\xa0\xdf\xbf", 4, out), 0);
	CHECK(strcmp(out, "\xc2\xa0\xdf\xbf") == 0);
	CHECK_EQ(banner_of("\xef\xbf\xbf", 3, out), 0);
	CHECK(strcmp(out, "\xef\xbf\xbf") == 0);
	CHECK_EQ(banner_of("\xf4\x8f\xbf\xbf", 4, out), 0);
	CHECK(strcmp(out, "\xf4\x8f\xbf\xbf") == 0);
	CHECK_EQ(banner_of("\xe0\xa0\x80\xed\x9f\xbf\xee\x80\x80", 9, out), 0);
	CHECK(strcmp(out, "\xe0\xa0\x80\xed\x9f\xbf\xee\x80\x80") == 0);
	CHECK_EQ(banner_of("\xf0\x90\x80\x80", 4, out), 0);
	CHECK(strcmp(out, "\xf0\x90\x80\x80") == 0);

	/* A C1 control character (U+0080, U+009B, U+009F) is one '?'; U+00A0 right after the range is kept. */
	CHECK_EQ(banner_of("\xc2\x80", 2, out), 0);
	CHECK(strcmp(out, "?") == 0);
	CHECK_EQ(banner_of("a\xc2\x9b" "31m", 6, out), 0);
	CHECK(strcmp(out, "a?31m") == 0);
	CHECK_EQ(banner_of("\xc2\x9f", 2, out), 0);
	CHECK(strcmp(out, "?") == 0);

	/* Overlong forms of three and four bytes, one byte per '?'. */
	CHECK_EQ(banner_of("\xe0\x80\x80", 3, out), 0);
	CHECK(strcmp(out, "???") == 0);
	CHECK_EQ(banner_of("\xe0\x9f\xbf", 3, out), 0);
	CHECK(strcmp(out, "???") == 0);
	CHECK_EQ(banner_of("\xf0\x80\x80\x80", 4, out), 0);
	CHECK(strcmp(out, "????") == 0);
	CHECK_EQ(banner_of("\xf0\x8f\xbf\xbf", 4, out), 0);
	CHECK(strcmp(out, "????") == 0);

	/* Surrogates (U+D800 and U+DFFF) and values above U+10FFFF. */
	CHECK_EQ(banner_of("\xed\xa0\x80", 3, out), 0);
	CHECK(strcmp(out, "???") == 0);
	CHECK_EQ(banner_of("\xed\xbf\xbf", 3, out), 0);
	CHECK(strcmp(out, "???") == 0);
	CHECK_EQ(banner_of("\xf4\x90\x80\x80", 4, out), 0);
	CHECK(strcmp(out, "????") == 0);
	CHECK_EQ(banner_of("\xf7\xbf\xbf\xbf", 4, out), 0);
	CHECK(strcmp(out, "????") == 0);

	/* The third and fourth bytes of a sequence must be continuations: C0 and 7F and 0x00 after a good start. */
	CHECK_EQ(banner_of("\xe2\x82\xc0", 3, out), 0);
	CHECK(strcmp(out, "???") == 0);
	CHECK_EQ(banner_of("\xe2\x82\x7f", 3, out), 0);
	CHECK(strcmp(out, "???") == 0);
	CHECK_EQ(banner_of("\xf0\x9f\x98\xc0", 4, out), 0);
	CHECK(strcmp(out, "????") == 0);
	CHECK_EQ(banner_of("\xf0\x9f\x7f\x80", 4, out), 0);
	CHECK(strcmp(out, "????") == 0);

	/* A sequence that is cut short, or broken by an ordinary byte, loses its bytes one by one and keeps the rest. */
	CHECK_EQ(banner_of("ab\xe2\x82", 4, out), 0);
	CHECK(strcmp(out, "ab??") == 0);
	CHECK_EQ(banner_of("\xe2\x82" "A", 3, out), 0);
	CHECK(strcmp(out, "??A") == 0);
	CHECK_EQ(banner_of("\xf0\x9f\x98", 3, out), 0);
	CHECK(strcmp(out, "???") == 0);
	CHECK_EQ(banner_of("\xc3", 1, out), 0);
	CHECK(strcmp(out, "?") == 0);
	CHECK_EQ(banner_of("\xc3" "A", 2, out), 0);
	CHECK(strcmp(out, "?A") == 0);

	/* The capacity: the text is cut to capacity - 1 bytes at a character boundary. */
	wire_banner(&built, "abcdef", 6);
	CHECK_EQ(ssh_auth_parse_banner(hold(built.data, built.size), built.size, out, 4), 0);
	CHECK(strcmp(out, "abc") == 0);
	CHECK_EQ(ssh_auth_parse_banner(hold(built.data, built.size), built.size, out, 7), 0);
	CHECK(strcmp(out, "abcdef") == 0);
	CHECK_EQ(ssh_auth_parse_banner(hold(built.data, built.size), built.size, out, 6), 0);
	CHECK(strcmp(out, "abcde") == 0);
	CHECK_EQ(ssh_auth_parse_banner(hold(built.data, built.size), built.size, out, 1), 0);
	CHECK_EQ(out[0], 0);
	CHECK_EQ(ssh_auth_parse_banner(hold(built.data, built.size), built.size, out, 0), EINVAL);
	CHECK_EQ(ssh_auth_parse_banner(hold(built.data, built.size), built.size, NULL, 10), EINVAL);

	/* A multi-byte character that does not fit is dropped whole, not split. */
	wire_banner(&built, "ab\xe2\x82\xac" "c", 6);
	CHECK_EQ(ssh_auth_parse_banner(hold(built.data, built.size), built.size, out, 5), 0);
	CHECK(strcmp(out, "ab") == 0);
	CHECK_EQ(ssh_auth_parse_banner(hold(built.data, built.size), built.size, out, 6), 0);
	CHECK(strcmp(out, "ab\xe2\x82\xac") == 0);
	CHECK_EQ(ssh_auth_parse_banner(hold(built.data, built.size), built.size, out, 7), 0);
	CHECK(strcmp(out, "ab\xe2\x82\xac" "c") == 0);

	/* A replacement counts as one byte against the capacity. */
	wire_banner(&built, "\x01\x01\x01", 3);
	CHECK_EQ(ssh_auth_parse_banner(hold(built.data, built.size), built.size, out, 3), 0);
	CHECK(strcmp(out, "??") == 0);
	wire_banner(&built, "\xc2\x9b\xc2\x9b\xc2\x9b", 6);
	CHECK_EQ(ssh_auth_parse_banner(hold(built.data, built.size), built.size, out, 3), 0);
	CHECK(strcmp(out, "??") == 0);

	/* A banner longer than SSH_BANNER_MAX is cut to it, whatever the capacity. */
	memset(big, 'a', sizeof(big));
	wire_banner(&built, big, TEST_TEXT_MAX);
	error = ssh_auth_parse_banner(hold(built.data, built.size), built.size, result, sizeof(result));
	CHECK_EQ(error, 0);
	CHECK_EQ(strlen(result), SSH_BANNER_MAX);
	memset(big, 'a', sizeof(big));
	wire_banner(&built, big, SSH_BANNER_MAX);
	error = ssh_auth_parse_banner(hold(built.data, built.size), built.size, result, sizeof(result));
	CHECK_EQ(error, 0);
	CHECK_EQ(strlen(result), SSH_BANNER_MAX);
	wire_banner(&built, big, SSH_BANNER_MAX - 1);
	error = ssh_auth_parse_banner(hold(built.data, built.size), built.size, result, sizeof(result));
	CHECK_EQ(error, 0);
	CHECK_EQ(strlen(result), SSH_BANNER_MAX - 1);

	/* The cut is at capacity - 1 up to SSH_BANNER_MAX: capacities around SSH_BANNER_MAX + 1. */
	memset(big, 'a', sizeof(big));
	wire_banner(&built, big, TEST_TEXT_MAX);
	error = ssh_auth_parse_banner(hold(built.data, built.size), built.size, result, SSH_BANNER_MAX);
	CHECK_EQ(error, 0);
	CHECK_EQ(strlen(result), SSH_BANNER_MAX - 1);
	error = ssh_auth_parse_banner(hold(built.data, built.size), built.size, result, SSH_BANNER_MAX + 1);
	CHECK_EQ(error, 0);
	CHECK_EQ(strlen(result), SSH_BANNER_MAX);
	error = ssh_auth_parse_banner(hold(built.data, built.size), built.size, result, SSH_BANNER_MAX + 2);
	CHECK_EQ(error, 0);
	CHECK_EQ(strlen(result), SSH_BANNER_MAX);
	error = ssh_auth_parse_banner(hold(built.data, built.size), built.size, result, SSH_BANNER_MAX + 3);
	CHECK_EQ(error, 0);
	CHECK_EQ(strlen(result), SSH_BANNER_MAX);

	/* A character that would pass SSH_BANNER_MAX is dropped whole. */
	memset(big, 'a', sizeof(big));
	big[SSH_BANNER_MAX - 2] = (char)0xe2;
	big[SSH_BANNER_MAX - 1] = (char)0x82;
	big[SSH_BANNER_MAX] = (char)0xac;
	wire_banner(&built, big, SSH_BANNER_MAX + 1);
	error = ssh_auth_parse_banner(hold(built.data, built.size), built.size, result, sizeof(result));
	CHECK_EQ(error, 0);
	CHECK_EQ(strlen(result), SSH_BANNER_MAX - 2);

	/* The message and the language tag: every shorter prefix is malformed, with the text emptied. */
	wire_banner(&built, "hi", 2);
	for (length = 0; length < built.size; length++) {
		memset(out, 'x', sizeof(out));
		error = ssh_auth_parse_banner(hold(built.data, length), length, out, sizeof(out));
		CHECK_EQ(error, EINVAL);
		CHECK_EQ(out[0], 0);
	}

	/* A byte after the language tag is malformed. */
	wire_u8(&built, 0);
	CHECK_EQ(ssh_auth_parse_banner(hold(built.data, built.size), built.size, out, sizeof(out)), EINVAL);

	/* A banner without the language string is malformed. */
	wire_reset(&built);
	wire_u8(&built, 53);
	wire_text(&built, "hi");
	CHECK_EQ(ssh_auth_parse_banner(hold(built.data, built.size), built.size, out, sizeof(out)), EINVAL);

	/* An empty language tag is accepted, and another message number is refused. */
	wire_reset(&built);
	wire_u8(&built, 53);
	wire_text(&built, "hi");
	wire_text(&built, "");
	CHECK_EQ(ssh_auth_parse_banner(hold(built.data, built.size), built.size, out, sizeof(out)), 0);
	CHECK(strcmp(out, "hi") == 0);
	built.data[0] = 52;
	CHECK_EQ(ssh_auth_parse_banner(hold(built.data, built.size), built.size, out, sizeof(out)), EINVAL);
}

/* Reports whether an error is one of the values a parser may return. */
static int
fuzz_error_ok(
	int error,
	int third)
{
	/* Success and EINVAL are always allowed. */
	if (error == 0 || error == EINVAL)
		return 1;

	/* The parser's own third value is allowed when it has one. */
	if (third != 0 && error == third)
		return 1;

	/* Nothing else is. */
	return 0;
}

/* Reads random changes of valid messages, and random bytes, with every parser. */
static void
test_fuzz(
	void)
{
	static struct wire templates[6];
	static uint8_t message[512];
	struct ssh_auth_request request;
	char text[64];
	char methods[64];
	int partial;
	unsigned long iteration;
	unsigned long bad;
	unsigned long accepted;
	size_t size;
	size_t position;
	size_t count;
	size_t index;
	int which;
	int mode;
	int error;
	int good;

	/* Builds the valid messages the changes start from: none, query, signed form, password, change form, unknown. */
	wire_request_head(&templates[0], "alice", "ssh-connection", "none");
	wire_request_head(&templates[1], "alice", "ssh-connection", "publickey");
	wire_publickey_fields(&templates[1], 0, "ssh-ed25519", t1_public);
	wire_request_head(&templates[2], "alice", "ssh-connection", "publickey");
	wire_publickey_fields(&templates[2], 1, "ssh-ed25519", t2_public);
	wire_signature(&templates[2], 0x42);
	wire_password_request(&templates[3], "alice", 0, "hunter2", NULL);
	wire_password_request(&templates[4], "alice", 1, "hunter2", "hunter3");
	wire_request_head(&templates[5], "alice", "ssh-connection", "hostbased");
	wire_text(&templates[5], "ssh-rsa");

	random_state = 0x2545f491UL;
	bad = 0;
	accepted = 0;

	/* Tries random changes of the requests: one to three bytes changed, and the text maybe cut. */
	for (iteration = 0; iteration < TEST_FUZZ_COUNT; iteration++) {
		which = (int)(random_next() % 6U);
		size = templates[which].size;
		memcpy(message, templates[which].data, size);
		mode = (int)(random_next() & 3U);
		count = 1U + (size_t)(random_next() % 3U);
		for (index = 0; index < count; index++) {
			position = (size_t)(random_next() % size);
			message[position] = (uint8_t)(message[position] ^ (uint8_t)((random_next() & 0xffU) | 1U));
		}

		if (mode == 1 || mode == 3)
			size = (size_t)(random_next() % (size + 1U));

		memset(&request, 0x5a, sizeof(request));
		error = parse_request_of(message, size, &request);

		/* The result is 0 or EINVAL; a failure leaves a zero request, a success a consistent one. */
		good = fuzz_error_ok(error, 0);
		if (error == 0) {
			accepted++;
			if (strlen(request.user) > SSH_USER_MAX)
				good = 0;
			if (strlen(request.service) > SSH_NAME_MAX)
				good = 0;
			if (strlen(request.algorithm) > SSH_NAME_MAX)
				good = 0;
			if (request.method == SSH_AUTH_PUBLICKEY) {
				if (request.key_blob < held || request.key_blob_size > size)
					good = 0;
				else if ((size_t)(request.key_blob - held) + request.key_blob_size > size)
					good = 0;
				if (request.has_signature != 0) {
					if (request.signed_size > size)
						good = 0;
					if (request.signature < held || request.signature_size > size)
						good = 0;
					else if ((size_t)(request.signature - held) + request.signature_size > size)
						good = 0;
					if (request.signed_size + 4U + request.signature_size != size)
						good = 0;
				}

				if (request.supported != 0 && strcmp(request.algorithm, "ssh-ed25519") != 0)
					good = 0;
			}
		} else if (!request_is_zero(&request)) {
			good = 0;
		}

		if (!good)
			bad++;
	}

	/* No result was wrong, and the changes both kept and broke messages. */
	CHECK_EQ(bad, 0);
	CHECK(accepted > 1000);
	CHECK(accepted < TEST_FUZZ_COUNT - 1000);

	/* Tries random bytes, short and long, with every other parser. */
	bad = 0;
	for (iteration = 0; iteration < TEST_FUZZ_COUNT; iteration++) {
		size = (size_t)(random_next() % 100U);
		for (index = 0; index < size; index++)
			message[index] = (uint8_t)(random_next() & 0xffU);

		/* A message number the parser accepts makes more inputs reach the fields. */
		if (size > 0) {
			which = (int)(random_next() % 5U);
			if (which == 0)
				message[0] = 5;
			else if (which == 1)
				message[0] = 51;
			else if (which == 2)
				message[0] = 53;
			else if (which == 3)
				message[0] = 60;
			else
				message[0] = 50;
		}

		if (size > 4 && (random_next() & 1U) != 0) {
			message[1] = 0;
			message[2] = 0;
			message[3] = 0;
			message[4] = (uint8_t)(random_next() % 40U);
		}

		/* Each parser returns one of its own values and stays inside its buffers. */
		error = ssh_auth_parse_service(hold(message, size), size, text);
		if (!fuzz_error_ok(error, EMSGSIZE))
			bad++;
		error = ssh_auth_parse_request(hold(message, size), size, &request);
		if (!fuzz_error_ok(error, 0))
			bad++;
		error = ssh_auth_parse_failure(hold(message, size), size, methods, sizeof(methods), &partial);
		if (!fuzz_error_ok(error, EMSGSIZE))
			bad++;
		error = ssh_auth_parse_pk_ok(hold(message, size), size, t1_public);
		if (!fuzz_error_ok(error, EPROTO))
			bad++;
		error = ssh_auth_parse_banner(hold(message, size), size, text, sizeof(text));
		if (!fuzz_error_ok(error, 0))
			bad++;
		if (error == 0 && strlen(text) >= sizeof(text))
			bad++;
	}

	CHECK_EQ(bad, 0);

	/* A banner of random bytes comes out with no control character but LF and TAB, and valid UTF-8 only. */
	bad = 0;
	for (iteration = 0; iteration < TEST_FUZZ_COUNT / 10UL; iteration++) {
		size = (size_t)(random_next() % 60U);
		for (index = 0; index < size; index++)
			message[index] = (uint8_t)(random_next() & 0xffU);
		wire_banner(&built, message, size);
		error = ssh_auth_parse_banner(hold(built.data, built.size), built.size, text, sizeof(text));
		if (error != 0) {
			bad++;
			continue;
		}

		for (index = 0; text[index] != '\0'; index++) {
			unsigned char byte;

			byte = (unsigned char)text[index];
			if (byte < 0x20U && byte != 0x0aU && byte != 0x09U)
				bad++;
			if (byte == 0x7fU)
				bad++;
			if (byte == 0xc0U || byte == 0xc1U || byte >= 0xf5U)
				bad++;
		}
	}

	CHECK_EQ(bad, 0);
}

/* Runs every test and prints the last line. */
int
main(
	void)
{
	int status;

	/* Prepares the keys and texts. */
	random_state = 1UL;
	setup_keys();

	/* Runs the groups of checks. */
	test_service_put();
	test_service_parse();
	test_request_put();
	test_signed();
	test_request_parse();
	test_request_names();
	test_failure_parse();
	test_pk_ok_parse();
	test_banner();
	test_fuzz();

	/* Frees the last held message, then reports. */
	free(held);
	held = NULL;
	status = check_finish("auth");
	return status;
}
