/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The messages and the arithmetic of the key exchange.
 */

#include <string.h>

#include "ssh-kex.h"

/* The number of name-lists in a KEXINIT (RFC 4253 7.1). */
#define KEX_LIST_COUNT		10U

/* The positions of the name-lists in struct ssh_kexinit (RFC 4253 7.1 order). */
#define KEX_LIST_KEX		0U
#define KEX_LIST_HOSTKEY	1U
#define KEX_LIST_CIPHER_CLIENT	2U
#define KEX_LIST_CIPHER_SERVER	3U
#define KEX_LIST_COMPRESSION_CLIENT	6U
#define KEX_LIST_COMPRESSION_SERVER	7U

/* The name of the compression these programs use (RFC 4253 6.2). */
#define KEX_COMPRESSION_NAME	"none"

/* The most key material one derivation gives: eight blocks of the hash. */
#define KEX_DERIVE_MAX		256U

/*
 * The kex_algorithms lists this side sends.
 *
 * Values referenced:
 *   draft-ietf-sshm-mlkem-hybrid-kex-10 section 2.3: the method name
 *   mlkem768x25519-sha256.
 *   draft-ietf-sshm-strict-kex-02 section 3.1: the markers "kex-strict-c"
 *   and "kex-strict-s" and their pre-standard spellings with the suffix
 *   "-v00@openssh.com".
 * The markers are sent only in the initial KEXINIT.
 */
static const char kex_list_client_initial[] =
	SSH_KEX_NAME "," SSH_KEX_STRICT_CLIENT "," SSH_KEX_STRICT_CLIENT_OLD;
static const char kex_list_server_initial[] =
	SSH_KEX_NAME "," SSH_KEX_STRICT_SERVER "," SSH_KEX_STRICT_SERVER_OLD;

static int kex_first_is(const char *list, size_t size, const char *name);
static int kex_hash_string(struct ssh_sha256 *hash, const uint8_t *data, size_t size);
static int kex_hash_fields(struct ssh_sha256 *hash, const struct ssh_kex_hash_input *input);
static int kex_derive_feed(struct ssh_sha256 *hash, const uint8_t *k, const uint8_t *h, const uint8_t *session_id, char letter, const uint8_t *previous, size_t previous_size);
static int kex_derive_block(const uint8_t *k, const uint8_t *h, const uint8_t *session_id, char letter, const uint8_t *previous, size_t previous_size, uint8_t *block);
static int kex_offered(const struct ssh_kexinit *peer, unsigned index, const char *name);

/*
 * Appends a KEXINIT payload (message number, 16 random cookie bytes from
 * ssh_random, the ten lists above for server 1 or 0, with the strict
 * markers when initial is 1, first_kex_packet_follows FALSE, reserved
 * uint32 0) to out.
 */
int
ssh_kexinit_put(
	int server,
	int initial,
	struct ssh_buf *out)
{
	uint8_t cookie[SSH_KEX_COOKIE];
	const char *kex_list;
	int error;

	/* Refuses to write into a buffer that has already failed. */
	error = ssh_buf_error(out);
	if (error != 0)
		return error;

	/* Draws the cookie before anything is written, so a failure leaves the buffer untouched. */
	error = ssh_random(cookie, sizeof(cookie));
	if (error != 0)
		return EIO;

	/* Picks the key exchange list: the markers of this side go in only with the initial exchange. */
	if (initial == 0) {
		kex_list = SSH_KEX_NAME;
	} else if (server != 0) {
		kex_list = kex_list_server_initial;
	} else {
		kex_list = kex_list_client_initial;
	}

	/* Writes the message number and the cookie. */
	ssh_buf_put_u8(out, SSH_MSG_KEXINIT);
	ssh_buf_put_bytes(out, cookie, sizeof(cookie));

	/* Writes the key exchange and host key lists. */
	ssh_buf_put_cstring(out, kex_list);
	ssh_buf_put_cstring(out, SSH_KEX_HOSTKEY_NAME);

	/* Writes the encryption lists, client to server then server to client. */
	ssh_buf_put_cstring(out, SSH_CIPHER_NAME);
	ssh_buf_put_cstring(out, SSH_CIPHER_NAME);

	/* Writes the MAC lists, empty because the cipher authenticates by itself. */
	ssh_buf_put_cstring(out, "");
	ssh_buf_put_cstring(out, "");

	/* Writes the compression lists. */
	ssh_buf_put_cstring(out, KEX_COMPRESSION_NAME);
	ssh_buf_put_cstring(out, KEX_COMPRESSION_NAME);

	/* Writes the language lists, empty. */
	ssh_buf_put_cstring(out, "");
	ssh_buf_put_cstring(out, "");

	/* Writes first_kex_packet_follows as FALSE and the reserved field as zero. */
	ssh_buf_put_bool(out, 0);
	ssh_buf_put_u32(out, 0);

	/* Reports the first failure the buffer recorded while it was written. */
	error = ssh_buf_error(out);
	if (error != 0)
		return error;

	/* Succeeded: the KEXINIT payload is in out. */
	return 0;
}

/*
 * Reads a peer's KEXINIT payload of size bytes (message number
 * included).
 */
int
ssh_kexinit_parse(
	const uint8_t *payload,
	size_t size,
	struct ssh_kexinit *kexinit)
{
	struct ssh_reader reader;
	struct ssh_kexinit parsed;
	unsigned index;
	uint8_t message;
	int error;

	/* Starts with nothing, which is what an error reports. */
	memset(&parsed, 0, sizeof(parsed));
	memset(kexinit, 0, sizeof(*kexinit));

	/* Refuses a payload longer than a KEXINIT may be. */
	if (size > SSH_KEXINIT_MAX)
		return EMSGSIZE;

	/* Reads the message number. */
	ssh_reader_init(&reader, payload, size);
	message = ssh_reader_get_u8(&reader);
	if (message != SSH_MSG_KEXINIT)
		return EINVAL;

	/* Skips the cookie. */
	(void)ssh_reader_get_bytes(&reader, SSH_KEX_COOKIE);

	/* Reads the ten name-lists, each checked as a well-formed list. */
	for (index = 0; index < KEX_LIST_COUNT; index++) {
		ssh_reader_get_namelist(&reader, &parsed.list[index], &parsed.list_size[index]);
	}

	/* Reads first_kex_packet_follows and skips the reserved field. */
	parsed.first_kex_packet_follows = ssh_reader_get_bool(&reader);
	(void)ssh_reader_get_u32(&reader);

	/* Demands that the payload ends exactly here. */
	error = ssh_reader_finish(&reader);
	if (error != 0)
		return error;

	/* Succeeded: hands out the lists, which point into payload. */
	*kexinit = parsed;
	return 0;
}

/*
 * Decides the algorithms from the peer's KEXINIT.
 */
int
ssh_kex_choose(
	int server,
	int initial,
	const struct ssh_kexinit *peer,
	struct ssh_kex_choice *choice,
	const char **kind)
{
	const char *marker;
	const char *marker_old;
	int strict;
	int ignore_next;
	int found;

	/* Starts with no failing kind named. */
	if (kind != NULL)
		*kind = NULL;

	/* The peer must offer the key exchange method. */
	found = kex_offered(peer, KEX_LIST_KEX, SSH_KEX_NAME);
	if (found == 0) {
		if (kind != NULL)
			*kind = "kex_algorithms";
		return ENOTSUP;
	}

	/* The peer must offer the host key algorithm. */
	found = kex_offered(peer, KEX_LIST_HOSTKEY, SSH_KEX_HOSTKEY_NAME);
	if (found == 0) {
		if (kind != NULL)
			*kind = "server_host_key_algorithms";
		return ENOTSUP;
	}

	/* The peer must offer the cipher in both directions. */
	found = kex_offered(peer, KEX_LIST_CIPHER_CLIENT, SSH_CIPHER_NAME);
	if (found == 0) {
		if (kind != NULL)
			*kind = "encryption_algorithms_client_to_server";
		return ENOTSUP;
	}

	/* Checks that the peer offers the cipher for server to client traffic. */
	found = kex_offered(peer, KEX_LIST_CIPHER_SERVER, SSH_CIPHER_NAME);
	if (found == 0) {
		if (kind != NULL)
			*kind = "encryption_algorithms_server_to_client";
		return ENOTSUP;
	}

	/* The peer must offer no compression in both directions; the MAC lists are not looked at. */
	found = kex_offered(peer, KEX_LIST_COMPRESSION_CLIENT, KEX_COMPRESSION_NAME);
	if (found == 0) {
		if (kind != NULL)
			*kind = "compression_algorithms_client_to_server";
		return ENOTSUP;
	}

	/* Checks that the peer offers the no-compression method for server to client traffic. */
	found = kex_offered(peer, KEX_LIST_COMPRESSION_SERVER, KEX_COMPRESSION_NAME);
	if (found == 0) {
		if (kind != NULL)
			*kind = "compression_algorithms_server_to_client";
		return ENOTSUP;
	}

	/* A later exchange keeps what the initial exchange decided. */
	strict = choice->strict;

	/* In the initial exchange the peer's marker of its own side switches strict key exchange on. */
	if (initial != 0) {
		if (server != 0) {
			marker = SSH_KEX_STRICT_CLIENT;
			marker_old = SSH_KEX_STRICT_CLIENT_OLD;
		} else {
			marker = SSH_KEX_STRICT_SERVER;
			marker_old = SSH_KEX_STRICT_SERVER_OLD;
		}

		/* Either spelling of the marker pairs with the two this side sends. */
		strict = 0;
		found = kex_offered(peer, KEX_LIST_KEX, marker);
		if (found != 0)
			strict = 1;
		found = kex_offered(peer, KEX_LIST_KEX, marker_old);
		if (found != 0)
			strict = 1;

		/* A peer without a marker is refused: strict key exchange is required. */
		if (strict == 0) {
			if (kind != NULL)
				*kind = "kex_algorithms (strict key exchange)";
			return EPROTO;
		}
	}

	/* A guessed packet is right only when the peer's first key exchange and host key algorithms are the ones chosen. */
	ignore_next = 0;
	if (peer->first_kex_packet_follows != 0) {
		ignore_next = 1;
		found = kex_first_is(peer->list[KEX_LIST_KEX], peer->list_size[KEX_LIST_KEX], SSH_KEX_NAME);
		if (found != 0) {
			found = kex_first_is(peer->list[KEX_LIST_HOSTKEY], peer->list_size[KEX_LIST_HOSTKEY], SSH_KEX_HOSTKEY_NAME);
			if (found != 0)
				ignore_next = 0;
		}
	}

	/* Succeeded: records the decision. */
	choice->strict = strict;
	choice->ignore_next = ignore_next;
	return 0;
}

/*
 * Appends SSH_MSG_KEX_HYBRID_INIT with C_INIT (SSH_HYBRID_INIT_SIZE
 * bytes) as a string to out.
 */
void
ssh_kex_put_hybrid_init(
	const uint8_t *c_init,
	struct ssh_buf *out)
{
	/* Writes the message number and the client's value. */
	ssh_buf_put_u8(out, SSH_MSG_KEX_HYBRID_INIT);
	ssh_buf_put_string(out, c_init, SSH_HYBRID_INIT_SIZE);
}

/*
 * Reads SSH_MSG_KEX_HYBRID_INIT: *c_init and *c_init_size point at the
 * string inside payload.
 */
int
ssh_kex_parse_hybrid_init(
	const uint8_t *payload,
	size_t size,
	const uint8_t **c_init,
	size_t *c_init_size)
{
	struct ssh_reader reader;
	const uint8_t *value;
	size_t value_size;
	uint8_t message;
	int error;

	/* Starts with nothing, which is what an error reports. */
	*c_init = NULL;
	*c_init_size = 0;

	/* Reads the message number. */
	ssh_reader_init(&reader, payload, size);
	message = ssh_reader_get_u8(&reader);
	if (message != SSH_MSG_KEX_HYBRID_INIT)
		return EINVAL;

	/* Reads the client's value. */
	ssh_reader_get_string(&reader, &value, &value_size);

	/* Demands that the payload ends exactly here. */
	error = ssh_reader_finish(&reader);
	if (error != 0)
		return error;

	/* Succeeded: hands out the string, which points into payload. */
	*c_init = value;
	*c_init_size = value_size;
	return 0;
}

/*
 * Appends SSH_MSG_KEX_HYBRID_REPLY: string K_S (the host key blob),
 * string S_REPLY, string signature (the signature blob over H), to out.
 */
void
ssh_kex_put_hybrid_reply(
	const uint8_t *host_key,
	size_t host_key_size,
	const uint8_t *s_reply,
	const uint8_t *signature,
	size_t signature_size,
	struct ssh_buf *out)
{
	/* Writes the message number, the host key, the server's value and the signature. */
	ssh_buf_put_u8(out, SSH_MSG_KEX_HYBRID_REPLY);
	ssh_buf_put_string(out, host_key, host_key_size);
	ssh_buf_put_string(out, s_reply, SSH_HYBRID_REPLY_SIZE);
	ssh_buf_put_string(out, signature, signature_size);
}

/*
 * Reads SSH_MSG_KEX_HYBRID_REPLY: pointers into payload for K_S, S_REPLY
 * and the signature blob.
 */
int
ssh_kex_parse_hybrid_reply(
	const uint8_t *payload,
	size_t size,
	const uint8_t **host_key,
	size_t *host_key_size,
	const uint8_t **s_reply,
	size_t *s_reply_size,
	const uint8_t **signature,
	size_t *signature_size)
{
	struct ssh_reader reader;
	const uint8_t *key_bytes;
	size_t key_size;
	const uint8_t *reply_bytes;
	size_t reply_size;
	const uint8_t *signature_bytes;
	size_t signature_length;
	uint8_t message;
	int error;

	/* Starts with nothing, which is what an error reports. */
	*host_key = NULL;
	*host_key_size = 0;
	*s_reply = NULL;
	*s_reply_size = 0;
	*signature = NULL;
	*signature_size = 0;

	/* Reads the message number. */
	ssh_reader_init(&reader, payload, size);
	message = ssh_reader_get_u8(&reader);
	if (message != SSH_MSG_KEX_HYBRID_REPLY)
		return EINVAL;

	/* Reads the host key, the server's value and the signature. */
	ssh_reader_get_string(&reader, &key_bytes, &key_size);
	ssh_reader_get_string(&reader, &reply_bytes, &reply_size);
	ssh_reader_get_string(&reader, &signature_bytes, &signature_length);

	/* Demands that the payload ends exactly here. */
	error = ssh_reader_finish(&reader);
	if (error != 0)
		return error;

	/* Succeeded: hands out the three strings, which point into payload. */
	*host_key = key_bytes;
	*host_key_size = key_size;
	*s_reply = reply_bytes;
	*s_reply_size = reply_size;
	*signature = signature_bytes;
	*signature_size = signature_length;
	return 0;
}

/*
 * Computes the exchange hash H = SHA-256(string V_C, string V_S, string
 * I_C, string I_S, string K_S, string C_INIT, string S_REPLY, string K)
 * (draft-ietf-sshm-mlkem-hybrid-kex-10 2.5) into h (32 bytes).
 */
int
ssh_kex_hash(
	const struct ssh_kex_hash_input *input,
	uint8_t *h)
{
	struct ssh_sha256 hash;
	int error;

	/* Starts the hash. */
	error = ssh_sha256_init(&hash);
	if (error != 0) {
		ssh_wipe(h, SSH_KEX_HASH_SIZE);
		return error;
	}

	/* Feeds the eight strings. */
	error = kex_hash_fields(&hash, input);
	if (error != 0) {
		ssh_sha256_free(&hash);
		ssh_wipe(h, SSH_KEX_HASH_SIZE);
		return error;
	}

	/* Finishes the hash (which frees the computation) into h. */
	error = ssh_sha256_final(&hash, h);
	if (error != 0) {
		ssh_wipe(h, SSH_KEX_HASH_SIZE);
		return error;
	}

	/* Succeeded: h holds the exchange hash. */
	return 0;
}

/*
 * Derives size bytes of key material for one letter (RFC 4253 7.2): with
 * K encoded as a string (draft-ietf-sshm-mlkem-hybrid-kex-10 2.5: uint32
 * 32, then the 32 bytes), K1 = HASH(K || H || letter || session_id), Kn
 * = HASH(K || H || K1 || ... || K(n-1)), and the output is the first size
 * bytes of K1 || K2 || ...
 */
int
ssh_kex_derive(
	const uint8_t *k,
	const uint8_t *h,
	const uint8_t *session_id,
	char letter,
	uint8_t *out,
	size_t size)
{
	uint8_t material[KEX_DERIVE_MAX];
	size_t blocks;
	size_t index;
	size_t made;
	int error;

	/* Refuses a letter outside the six of RFC 4253 7.2. */
	if (letter < 'A' || letter > 'F')
		return EINVAL;

	/* Refuses a size outside 1 to 256 bytes. */
	if (size == 0 || size > KEX_DERIVE_MAX)
		return EINVAL;

	/* Counts the blocks of the hash that cover the size. */
	blocks = (size + SSH_KEX_HASH_SIZE - 1U) / SSH_KEX_HASH_SIZE;

	/* Makes each block from the blocks before it. */
	for (index = 0; index < blocks; index++) {
		made = index * SSH_KEX_HASH_SIZE;
		error = kex_derive_block(k, h, session_id, letter, material, made, material + made);
		if (error != 0) {
			ssh_wipe(material, sizeof(material));
			ssh_wipe(out, size);
			return error;
		}
	}

	/* Hands out the first size bytes and wipes the material. */
	memcpy(out, material, size);
	ssh_wipe(material, sizeof(material));

	/* Succeeded: out holds the key material. */
	return 0;
}

/* Reports whether the first name of the name-list of size bytes at list is exactly name. */
static int
kex_first_is(
	const char *list,
	size_t size,
	const char *name)
{
	size_t name_length;
	size_t first_length;
	int difference;

	/* A missing or empty list has no first name. */
	if (list == NULL)
		return 0;
	if (size == 0)
		return 0;

	/* Finds the end of the first name. */
	first_length = 0;
	while (first_length < size && list[first_length] != ',')
		first_length++;

	/* A first name of another length is another name. */
	name_length = strlen(name);
	if (first_length != name_length)
		return 0;

	/* Compares the first name whole with the name asked for. */
	difference = memcmp(list, name, name_length);
	if (difference != 0)
		return 0;

	/* The first name is the one asked for. */
	return 1;
}

/* Reports whether the peer's name-list number index contains name. */
static int
kex_offered(
	const struct ssh_kexinit *peer,
	unsigned index,
	const char *name)
{
	int found;

	/* Looks the name up in the list. */
	found = ssh_namelist_contains(peer->list[index], peer->list_size[index], name);

	/* Reports whether it is there. */
	if (found != 0)
		return 1;
	return 0;
}

/* Feeds one string (a uint32 length, then the bytes) into the hash; on a failure the computation is not freed here. */
static int
kex_hash_string(
	struct ssh_sha256 *hash,
	const uint8_t *data,
	size_t size)
{
	uint8_t length[4];
	int error;

	/* A string longer than a uint32 can count cannot be encoded. */
	if (size > 0xffffffffU)
		return EINVAL;

	/* Feeds the length. */
	ssh_store_be32(length, (uint32_t)size);
	error = ssh_sha256_update(hash, length, sizeof(length));
	if (error != 0)
		return error;

	/* Feeds the bytes. */
	error = ssh_sha256_update(hash, data, size);
	if (error != 0)
		return error;

	/* Succeeded: the string is in the hash. */
	return 0;
}

/* Feeds the eight strings of the exchange hash, in the order the draft gives, into the hash. */
static int
kex_hash_fields(
	struct ssh_sha256 *hash,
	const struct ssh_kex_hash_input *input)
{
	int error;

	/* Feeds the two identification lines. */
	error = kex_hash_string(hash, input->version_client, input->version_client_size);
	if (error != 0)
		return error;
	error = kex_hash_string(hash, input->version_server, input->version_server_size);
	if (error != 0)
		return error;

	/* Feeds the two KEXINIT payloads. */
	error = kex_hash_string(hash, input->kexinit_client, input->kexinit_client_size);
	if (error != 0)
		return error;
	error = kex_hash_string(hash, input->kexinit_server, input->kexinit_server_size);
	if (error != 0)
		return error;

	/* Feeds the host key and the two values of the hybrid exchange. */
	error = kex_hash_string(hash, input->host_key, input->host_key_size);
	if (error != 0)
		return error;
	error = kex_hash_string(hash, input->c_init, input->c_init_size);
	if (error != 0)
		return error;
	error = kex_hash_string(hash, input->s_reply, input->s_reply_size);
	if (error != 0)
		return error;

	/* Feeds the shared secret as a string of 32 bytes, not as an mpint. */
	error = kex_hash_string(hash, input->k, SSH_KEX_HASH_SIZE);
	if (error != 0)
		return error;

	/* Succeeded: the eight strings are in the hash. */
	return 0;
}

/*
 * Feeds the input of one block of key material into the hash: K as a
 * string, then H, then the letter and the session identifier for the first
 * block (previous_size 0), or the blocks made so far for a later one.
 */
static int
kex_derive_feed(
	struct ssh_sha256 *hash,
	const uint8_t *k,
	const uint8_t *h,
	const uint8_t *session_id,
	char letter,
	const uint8_t *previous,
	size_t previous_size)
{
	uint8_t letter_byte;
	int error;

	/* Feeds K as a string of 32 bytes, then H. */
	error = kex_hash_string(hash, k, SSH_KEX_HASH_SIZE);
	if (error != 0)
		return error;
	error = ssh_sha256_update(hash, h, SSH_KEX_HASH_SIZE);
	if (error != 0)
		return error;

	/* A later block continues with the blocks made so far. */
	if (previous_size != 0) {
		error = ssh_sha256_update(hash, previous, previous_size);
		if (error != 0)
			return error;

		/* Succeeded: the input of a later block is in the hash. */
		return 0;
	}

	/* The first block continues with the letter and the session identifier. */
	letter_byte = (uint8_t)letter;
	error = ssh_sha256_update(hash, &letter_byte, 1);
	if (error != 0)
		return error;
	error = ssh_sha256_update(hash, session_id, SSH_KEX_HASH_SIZE);
	if (error != 0)
		return error;

	/* Succeeded: the input of the first block is in the hash. */
	return 0;
}

/* Makes one block of key material, HASH(K || H || letter || session_id) for the first block or HASH(K || H || previous) for a later one. */
static int
kex_derive_block(
	const uint8_t *k,
	const uint8_t *h,
	const uint8_t *session_id,
	char letter,
	const uint8_t *previous,
	size_t previous_size,
	uint8_t *block)
{
	struct ssh_sha256 hash;
	int error;

	/* Starts the hash. */
	error = ssh_sha256_init(&hash);
	if (error != 0)
		return error;

	/* Feeds the input of the block. */
	error = kex_derive_feed(&hash, k, h, session_id, letter, previous, previous_size);
	if (error != 0) {
		ssh_sha256_free(&hash);
		return error;
	}

	/* Finishes the hash (which frees the computation) into the block. */
	error = ssh_sha256_final(&hash, block);
	if (error != 0)
		return error;

	/* Succeeded: the block is made. */
	return 0;
}
