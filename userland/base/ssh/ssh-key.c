/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ssh-ed25519 keys: blobs, signatures, public key lines, private key files, fingerprints.
 */

#include <string.h>

#include "ssh-key.h"

/* The length of the algorithm name "ssh-ed25519" in bytes. */
#define KEY_NAME_LENGTH		11U

/*
 * The most base64 characters accepted in the blob field of a public key line.
 *
 * A blob of SSH_KEY_BLOB_SIZE bytes is 68 characters; the limit keeps a
 * decode buffer small while leaving no room for any other length to be
 * a valid key.
 */
#define KEY_LINE_BASE64_MAX	72U

/* The decoded bytes the blob field of a public key line can hold (72 characters decode to 54 bytes). */
#define KEY_LINE_DECODED_MAX	54U

/* The cipher block size of "none" (OpenSSH PROTOCOL.key 3, 5), which the private section is padded to. */
#define KEY_PRIVATE_BLOCK	8U

/* The size of the agent encoding's second string for Ed25519: the seed followed by the public key. */
#define KEY_PRIVATE_PAIR_SIZE	(SSH_ED25519_SEED + SSH_ED25519_PUBLIC)

/* The length of the fixed prefix "SHA256:" of a fingerprint. */
#define KEY_FINGERPRINT_PREFIX_LENGTH	7U

/*
 * The magic that starts the decoded private key file, with its NUL byte.
 *
 * Values referenced: OpenSSH PROTOCOL.key section 1, AUTH_MAGIC
 * "openssh-key-v1" (the NUL is part of the byte[] that is written).
 */
static const char private_magic[15] = "openssh-key-v1";

static int key_check_name(const uint8_t *name, size_t size);
static void key_copy_comment(struct ssh_key *key, const char *text, size_t size);
static int key_is_blank(char byte);
static int key_text_is(const uint8_t *text, size_t size, const char *expected);
static int private_join_lines(const char *text, size_t size, struct ssh_buf *joined);
static int private_parse_decoded(const uint8_t *data, size_t size, struct ssh_key *key);
static int private_put_inner(const struct ssh_key *key, size_t comment_size, uint32_t checkint, struct ssh_buf *inner);
static int private_comment_size(const struct ssh_key *key, size_t *comment_size);

/*
 * Wipes the seed and clears the whole structure.
 */
void
ssh_key_clear(
	struct ssh_key *key)
{
	/* Overwrites the seed, the public key and the comment in a way the compiler keeps. */
	ssh_wipe(key, sizeof(*key));
}

/*
 * Makes a new key: a 32-byte seed from ssh_random and its public key
 * (ssh_ed25519_public).  comment (NULL for none) is copied, cut to fit.
 */
int
ssh_key_generate(
	struct ssh_key *key,
	const char *comment)
{
	int error;

	/* Starts from an empty key, so a failure leaves nothing behind. */
	ssh_key_clear(key);

	/* Draws the secret seed from the library's random generator. */
	error = ssh_random(key->seed, SSH_ED25519_SEED);
	if (error != 0) {
		ssh_key_clear(key);
		return EIO;
	}

	/* Derives the public key of the seed (RFC 8032 5.1.5). */
	error = ssh_ed25519_public(key->seed, key->public_key);
	if (error != 0) {
		ssh_key_clear(key);
		return EIO;
	}

	/* Keeps the comment, cut to what the structure holds. */
	if (comment != NULL)
		key_copy_comment(key, comment, strlen(comment));

	/* The key now holds a seed. */
	key->private = 1;

	/* Succeeded: a new private key. */
	return 0;
}

/*
 * Appends the public key blob (RFC 8709 4: string "ssh-ed25519", string
 * of the 32-byte key) to out.
 */
void
ssh_key_put_public_blob(
	const uint8_t *public_key,
	struct ssh_buf *out)
{
	/* Writes the algorithm name as a string. */
	ssh_buf_put_string(out, SSH_KEY_NAME, KEY_NAME_LENGTH);

	/* Writes the 32-byte public key as a string. */
	ssh_buf_put_string(out, public_key, SSH_ED25519_PUBLIC);
}

/*
 * Reads a public key blob of size bytes and writes the 32-byte key.
 *
 * The algorithm name is looked at first, so a blob of another algorithm is
 * ENOTSUP whatever follows its name.
 */
int
ssh_key_parse_public_blob(
	const uint8_t *blob,
	size_t size,
	uint8_t *public_key)
{
	struct ssh_reader reader;
	const uint8_t *name;
	size_t name_size;
	const uint8_t *key_bytes;
	size_t key_size;
	int error;

	/* Refuses bytes that are missing. */
	if (blob == NULL && size != 0)
		return EINVAL;

	/* Reads the algorithm name. */
	ssh_reader_init(&reader, blob, size);
	ssh_reader_get_string(&reader, &name, &name_size);
	error = ssh_reader_error(&reader);
	if (error != 0)
		return EINVAL;

	/* Tells another algorithm and a malformed name apart from ssh-ed25519. */
	error = key_check_name(name, name_size);
	if (error != 0)
		return error;

	/* Reads the key string. */
	ssh_reader_get_string(&reader, &key_bytes, &key_size);
	error = ssh_reader_error(&reader);
	if (error != 0)
		return EINVAL;

	/* The key of RFC 8032 5.1.5 is exactly 32 octets. */
	if (key_size != SSH_ED25519_PUBLIC)
		return EINVAL;

	/* Refuses bytes after the key. */
	error = ssh_reader_finish(&reader);
	if (error != 0)
		return EINVAL;

	/* Hands the key to the caller, only now that the whole blob is good. */
	memcpy(public_key, key_bytes, SSH_ED25519_PUBLIC);

	/* Succeeded: the blob held a valid ssh-ed25519 key. */
	return 0;
}

/*
 * Signs size bytes at data with key (which must be private; EINVAL
 * otherwise) and appends the signature blob (RFC 8709 6: string
 * "ssh-ed25519", string of the 64-byte signature) to out.
 *
 * When out cannot hold the blob, the error recorded in out (EMSGSIZE or
 * ENOMEM) is returned and nothing is appended.
 */
int
ssh_key_sign(
	const struct ssh_key *key,
	const uint8_t *data,
	size_t size,
	struct ssh_buf *out)
{
	uint8_t signature[SSH_ED25519_SIGNATURE];
	int error;

	/* Refuses a key that holds no seed. */
	if (key->private != 1)
		return EINVAL;

	/* Signs the data with the pure Ed25519 of RFC 8032 5.1.6. */
	error = ssh_ed25519_sign(key->seed, data, size, signature);
	if (error != 0) {
		ssh_wipe(signature, sizeof(signature));
		return EIO;
	}

	/* Makes room for the whole blob first, so a failure appends nothing. */
	error = ssh_buf_reserve(out, SSH_KEY_SIGNATURE_BLOB_SIZE);
	if (error != 0) {
		ssh_wipe(signature, sizeof(signature));
		return error;
	}

	/* Writes the algorithm name and the 64-byte signature as two strings (RFC 8709 6). */
	ssh_buf_put_string(out, SSH_KEY_NAME, KEY_NAME_LENGTH);
	ssh_buf_put_string(out, signature, SSH_ED25519_SIGNATURE);
	ssh_wipe(signature, sizeof(signature));

	/* Succeeded: the signature blob is appended. */
	return 0;
}

/*
 * Verifies a signature blob of blob_size bytes over size bytes at data
 * with a 32-byte public key.
 */
int
ssh_key_verify(
	const uint8_t *public_key,
	const uint8_t *data,
	size_t size,
	const uint8_t *blob,
	size_t blob_size)
{
	struct ssh_reader reader;
	const uint8_t *name;
	size_t name_size;
	const uint8_t *signature;
	size_t signature_size;
	int error;

	/* Refuses bytes that are missing. */
	if (blob == NULL && blob_size != 0)
		return EINVAL;

	/* Reads the algorithm name. */
	ssh_reader_init(&reader, blob, blob_size);
	ssh_reader_get_string(&reader, &name, &name_size);
	error = ssh_reader_error(&reader);
	if (error != 0)
		return EINVAL;

	/* Tells another algorithm and a malformed name apart from ssh-ed25519. */
	error = key_check_name(name, name_size);
	if (error != 0)
		return error;

	/* Reads the signature string. */
	ssh_reader_get_string(&reader, &signature, &signature_size);
	error = ssh_reader_error(&reader);
	if (error != 0)
		return EINVAL;

	/* The signature of RFC 8032 5.1.6 is exactly 64 octets. */
	if (signature_size != SSH_ED25519_SIGNATURE)
		return EINVAL;

	/* Refuses bytes after the signature. */
	error = ssh_reader_finish(&reader);
	if (error != 0)
		return EINVAL;

	/* Checks the signature (RFC 8032 5.1.7); EBADMSG says it does not verify. */
	error = ssh_ed25519_verify(public_key, data, size, signature);
	if (error != 0)
		return error;

	/* Succeeded: the signature is valid for the data. */
	return 0;
}

/*
 * Appends the one-line public key form "ssh-ed25519 <base64 of the blob>
 * <comment>\n" to out (the comment and its space left out when empty),
 * as a .pub file and an authorized_keys line hold it (sshd(8)
 * AUTHORIZED_KEYS FILE FORMAT).
 *
 * A comment that holds a CR or an LF would end the line early; it records
 * EINVAL in out and appends nothing.
 */
void
ssh_key_put_public_line(
	const uint8_t *public_key,
	const char *comment,
	struct ssh_buf *out)
{
	struct ssh_buf blob;
	char text[SSH_KEY_BLOB_SIZE * 2];
	size_t comment_size;
	size_t index;
	size_t text_size;
	int error;

	comment_size = 0;
	if (comment != NULL)
		comment_size = strlen(comment);

	/* Refuses a comment that would break the line in two. */
	for (index = 0; index < comment_size; index++) {
		if (comment[index] == '\n' || comment[index] == '\r') {
			if (out->error == 0)
				out->error = EINVAL;
			return;
		}
	}

	/* Builds the public key blob apart from out, so the base64 can be taken from it. */
	ssh_buf_init(&blob, SSH_KEY_BLOB_SIZE, 0);
	ssh_key_put_public_blob(public_key, &blob);
	error = ssh_buf_error(&blob);
	if (error != 0) {
		ssh_buf_free(&blob);
		if (out->error == 0)
			out->error = error;
		return;
	}

	/* Encodes the blob as base64 with padding (RFC 4648 4). */
	error = ssh_base64_encode(blob.data, blob.size, text, sizeof(text), 1);
	ssh_buf_free(&blob);
	if (error != 0) {
		if (out->error == 0)
			out->error = error;
		return;
	}
	text_size = strlen(text);

	/* Writes the type, a space and the base64 blob. */
	ssh_buf_put_bytes(out, SSH_KEY_NAME, KEY_NAME_LENGTH);
	ssh_buf_put_u8(out, ' ');
	ssh_buf_put_bytes(out, text, text_size);

	/* Writes a space and the comment when there is one. */
	if (comment_size != 0) {
		ssh_buf_put_u8(out, ' ');
		ssh_buf_put_bytes(out, comment, comment_size);
	}

	/* Ends the line. */
	ssh_buf_put_u8(out, '\n');
}

/*
 * Reads a public key in the one-line form from size bytes at text (one
 * line, without its LF): the type, one or more spaces or tabs, the
 * base64 blob, and optionally white space and a comment.
 *
 * Leading white space is skipped, and trailing white space (spaces, tabs
 * and a CR) is not part of the comment.  A NUL byte or an LF inside the
 * text is EINVAL.  key is written only when the line is good.
 */
int
ssh_key_parse_public_line(
	const char *text,
	size_t size,
	struct ssh_key *key)
{
	struct ssh_key found;
	uint8_t decoded[KEY_LINE_DECODED_MAX];
	uint8_t public_key[SSH_ED25519_PUBLIC];
	size_t decoded_size;
	size_t end;
	size_t position;
	size_t type_start;
	size_t blob_start;
	int error;

	/* Refuses bytes that are missing. */
	if (text == NULL && size != 0)
		return EINVAL;

	/* A line holds no NUL byte and no LF; either would hide text from a reader of C strings or end the line early. */
	for (position = 0; position < size; position++) {
		if (text[position] == '\0' || text[position] == '\n')
			return EINVAL;
	}

	/* Finds where the line really ends: trailing white space and a CR are not content. */
	end = size;
	while (end > 0 && (key_is_blank(text[end - 1]) || text[end - 1] == '\r'))
		end--;

	/* Skips the white space before the type. */
	position = 0;
	while (position < end && key_is_blank(text[position]))
		position++;

	/* Takes the type field: the characters up to the next white space. */
	type_start = position;
	while (position < end && !key_is_blank(text[position]))
		position++;
	if (position == type_start)
		return EINVAL;

	/* Another type is not this key (a host key line of another type is skipped by the caller). */
	if (!key_text_is((const uint8_t *)text + type_start, position - type_start, SSH_KEY_NAME))
		return ENOTSUP;

	/* Skips the white space between the type and the blob. */
	while (position < end && key_is_blank(text[position]))
		position++;

	/* Takes the blob field; a line without one is malformed. */
	blob_start = position;
	while (position < end && !key_is_blank(text[position]))
		position++;
	if (position == blob_start)
		return EINVAL;
	if (position - blob_start > KEY_LINE_BASE64_MAX)
		return EINVAL;

	/* Decodes the base64 blob. */
	error = ssh_base64_decode(text + blob_start, position - blob_start, decoded, sizeof(decoded), &decoded_size);
	if (error != 0)
		return EINVAL;

	/* Parses the blob; the type inside must be the one the line names, so another name there is malformed. */
	error = ssh_key_parse_public_blob(decoded, decoded_size, public_key);
	if (error != 0)
		return EINVAL;

	/* Skips the white space before the comment. */
	while (position < end && key_is_blank(text[position]))
		position++;

	/* Builds the result apart from the caller's key. */
	ssh_key_clear(&found);
	memcpy(found.public_key, public_key, SSH_ED25519_PUBLIC);
	found.private = 0;
	key_copy_comment(&found, text + position, end - position);

	/* Hands the key to the caller. */
	*key = found;

	/* Succeeded: the line held a valid ssh-ed25519 public key. */
	return 0;
}

/*
 * Reads an OpenSSH private key file of size bytes at text.
 *
 * The base64 lines are joined and decoded into secret buffers that are
 * wiped on every path; key is cleared first and again on an error.
 */
int
ssh_key_parse_private(
	const char *text,
	size_t size,
	struct ssh_key *key)
{
	struct ssh_buf joined;
	struct ssh_buf decoded;
	size_t decoded_capacity;
	size_t decoded_size;
	int error;

	/* Starts from an empty key, so any failure leaves nothing behind. */
	ssh_key_clear(key);

	/* The base64 text and the bytes it decodes to both hold the secret. */
	ssh_buf_init(&joined, SSH_KEY_FILE_MAX, 1);
	ssh_buf_init(&decoded, SSH_KEY_FILE_MAX, 1);

	/* Refuses a file larger than the limit, and bytes that are missing. */
	if (size > SSH_KEY_FILE_MAX) {
		error = EMSGSIZE;
		goto done;
	}
	if (text == NULL) {
		error = EINVAL;
		goto done;
	}

	/* Takes the base64 lines between the BEGIN line and the END line and joins them. */
	error = private_join_lines(text, size, &joined);
	if (error != 0)
		goto done;

	/* An armour with no base64 inside holds no key. */
	if (joined.size == 0) {
		error = EINVAL;
		goto done;
	}

	/* Makes room for the decoded bytes: three bytes for every four characters, with a group to spare. */
	decoded_capacity = (joined.size / 4U + 1U) * 3U;
	error = ssh_buf_reserve(&decoded, decoded_capacity);
	if (error != 0)
		goto done;

	/* Decodes the joined base64 (RFC 4648 4). */
	error = ssh_base64_decode((const char *)joined.data, joined.size, decoded.data, decoded_capacity, &decoded_size);
	if (error != 0) {
		error = EINVAL;
		goto done;
	}
	decoded.size = decoded_size;

	/* Parses the openssh-key-v1 structure and checks the key it holds. */
	error = private_parse_decoded(decoded.data, decoded_size, key);

done:
	/* Wipes and frees both buffers on every path. */
	ssh_buf_free(&joined);
	ssh_buf_free(&decoded);

	/* A failed parse leaves no part of a key behind. */
	if (error != 0) {
		ssh_key_clear(key);
		return error;
	}

	/* Succeeded: key holds the private key. */
	return 0;
}

/*
 * Appends key (private 1; EINVAL otherwise) to out as an OpenSSH private
 * key file in the layout ssh_key_parse_private reads, unencrypted,
 * checkint the given value twice, base64 wrapped at SSH_KEY_PRIVATE_WRAP
 * characters, each line ending with LF.
 *
 * The room for the whole file is reserved in out first, so a failure
 * appends nothing.  A comment without its NUL inside the array is EINVAL.
 */
int
ssh_key_put_private(
	const struct ssh_key *key,
	uint32_t checkint,
	struct ssh_buf *out)
{
	struct ssh_buf inner;
	struct ssh_buf body;
	struct ssh_buf armoured;
	uint8_t *space;
	size_t comment_size;
	size_t encoded_size;
	size_t line_count;
	size_t total;
	size_t position;
	size_t chunk;
	int error;

	/* Refuses a key that holds no seed. */
	if (key->private != 1)
		return EINVAL;

	/* Measures the comment, which must end inside its array. */
	error = private_comment_size(key, &comment_size);
	if (error != 0)
		return error;

	/* The private section, the whole decoded file and its base64 all hold the seed. */
	ssh_buf_init(&inner, SSH_KEY_FILE_MAX, 1);
	ssh_buf_init(&body, SSH_KEY_FILE_MAX, 1);
	ssh_buf_init(&armoured, SSH_KEY_FILE_MAX, 1);

	/* Builds the padded private section (PROTOCOL.key 3). */
	error = private_put_inner(key, comment_size, checkint, &inner);
	if (error != 0)
		goto done;

	/* Writes the header: magic, cipher none, KDF none, empty options, one key (PROTOCOL.key 1, 5). */
	ssh_buf_put_bytes(&body, private_magic, sizeof(private_magic));
	ssh_buf_put_cstring(&body, "none");
	ssh_buf_put_cstring(&body, "none");
	ssh_buf_put_string(&body, NULL, 0);
	ssh_buf_put_u32(&body, 1);

	/* Writes the public key blob as a string, then the private section as a string. */
	ssh_buf_put_u32(&body, SSH_KEY_BLOB_SIZE);
	ssh_key_put_public_blob(key->public_key, &body);
	ssh_buf_put_string(&body, inner.data, inner.size);
	error = ssh_buf_error(&body);
	if (error != 0)
		goto done;

	/* Encodes the whole file body as base64 with padding (RFC 4648 4), leaving the NUL out. */
	encoded_size = ssh_base64_encoded_size(body.size);
	space = ssh_buf_put_space(&armoured, encoded_size + 1U);
	if (space == NULL) {
		error = ssh_buf_error(&armoured);
		goto done;
	}
	error = ssh_base64_encode(body.data, body.size, (char *)space, encoded_size + 1U, 1);
	if (error != 0) {
		error = EIO;
		goto done;
	}
	armoured.size = armoured.size - 1U;

	/* Reserves the room for the BEGIN line, the wrapped base64 lines and the END line. */
	line_count = (encoded_size + SSH_KEY_PRIVATE_WRAP - 1U) / SSH_KEY_PRIVATE_WRAP;
	total = strlen(SSH_KEY_PRIVATE_BEGIN) + 1U + encoded_size + line_count + strlen(SSH_KEY_PRIVATE_END) + 1U;
	error = ssh_buf_reserve(out, total);
	if (error != 0)
		goto done;

	/* Writes the BEGIN line. */
	ssh_buf_put_bytes(out, SSH_KEY_PRIVATE_BEGIN, strlen(SSH_KEY_PRIVATE_BEGIN));
	ssh_buf_put_u8(out, '\n');

	/* Writes the base64 in lines of SSH_KEY_PRIVATE_WRAP characters, the last one shorter. */
	position = 0;
	while (position < encoded_size) {
		chunk = encoded_size - position;
		if (chunk > SSH_KEY_PRIVATE_WRAP)
			chunk = SSH_KEY_PRIVATE_WRAP;
		ssh_buf_put_bytes(out, armoured.data + position, chunk);
		ssh_buf_put_u8(out, '\n');
		position += chunk;
	}

	/* Writes the END line. */
	ssh_buf_put_bytes(out, SSH_KEY_PRIVATE_END, strlen(SSH_KEY_PRIVATE_END));
	ssh_buf_put_u8(out, '\n');
	error = ssh_buf_error(out);

done:
	/* Wipes and frees the working buffers on every path. */
	ssh_buf_free(&inner);
	ssh_buf_free(&body);
	ssh_buf_free(&armoured);

	/* Reports why the file could not be written. */
	if (error != 0)
		return error;

	/* Succeeded: the private key file is appended. */
	return 0;
}

/*
 * Writes the SHA256 fingerprint of a public key: "SHA256:" followed by
 * the base64 of SHA-256 of the public key blob without '=' padding, as
 * ssh-keygen -l and ssh print it, and a NUL.
 */
int
ssh_key_fingerprint(
	const uint8_t *public_key,
	char *out,
	size_t capacity)
{
	uint8_t blob[SSH_KEY_BLOB_SIZE];
	uint8_t digest[SSH_SHA256_SIZE];
	char text[64];
	size_t text_size;
	int error;

	/* Refuses a buffer too small for the text and its NUL. */
	if (out == NULL || capacity < SSH_KEY_FINGERPRINT_SIZE)
		return ENOBUFS;

	/* Builds the public key blob in place: string "ssh-ed25519", string of the key (RFC 8709 4). */
	ssh_store_be32(blob, KEY_NAME_LENGTH);
	memcpy(blob + 4U, SSH_KEY_NAME, KEY_NAME_LENGTH);
	ssh_store_be32(blob + 4U + KEY_NAME_LENGTH, SSH_ED25519_PUBLIC);
	memcpy(blob + 8U + KEY_NAME_LENGTH, public_key, SSH_ED25519_PUBLIC);

	/* Hashes the blob with SHA-256. */
	error = ssh_sha256(blob, sizeof(blob), digest);
	if (error != 0)
		return EIO;

	/* Encodes the digest as base64 without padding. */
	error = ssh_base64_encode(digest, sizeof(digest), text, sizeof(text), 0);
	if (error != 0)
		return EIO;
	text_size = strlen(text);

	/* Writes the prefix, the base64 and the NUL. */
	memcpy(out, "SHA256:", KEY_FINGERPRINT_PREFIX_LENGTH);
	memcpy(out + KEY_FINGERPRINT_PREFIX_LENGTH, text, text_size + 1U);

	/* Succeeded: the fingerprint is written. */
	return 0;
}

/*
 * Tells the name in a blob from the one this module supports.
 *
 * Returns 0 for "ssh-ed25519", EINVAL for a name that cannot be an
 * algorithm name (empty or longer than SSH_NAME_MAX, RFC 4251 6) and
 * ENOTSUP for any other name.
 */
static int
key_check_name(
	const uint8_t *name,
	size_t size)
{
	/* An algorithm name has 1 to SSH_NAME_MAX bytes. */
	if (size == 0 || size > SSH_NAME_MAX)
		return EINVAL;

	/* A name of another length or text is another algorithm. */
	if (!key_text_is(name, size, SSH_KEY_NAME))
		return ENOTSUP;

	/* Succeeded: the name is ssh-ed25519. */
	return 0;
}

/*
 * Copies at most SSH_KEY_COMMENT_MAX - 1 bytes of text into the comment of key and ends it with a NUL.
 */
static void
key_copy_comment(
	struct ssh_key *key,
	const char *text,
	size_t size)
{
	/* Cuts a comment that does not fit, keeping room for the NUL. */
	if (size > SSH_KEY_COMMENT_MAX - 1U)
		size = SSH_KEY_COMMENT_MAX - 1U;

	/* Copies the kept bytes and ends the string. */
	if (size > 0)
		memcpy(key->comment, text, size);
	key->comment[size] = '\0';
}

/*
 * Tells whether a byte separates the fields of a public key line (a space or a tab).
 */
static int
key_is_blank(
	char byte)
{
	/* A space and a tab separate the fields of the line. */
	if (byte == ' ')
		return 1;
	if (byte == '\t')
		return 1;

	/* Any other byte is part of a field. */
	return 0;
}

/*
 * Tells whether size bytes at text are exactly the NUL-terminated string expected.
 */
static int
key_text_is(
	const uint8_t *text,
	size_t size,
	const char *expected)
{
	size_t expected_size;

	/* A different length is a different text. */
	expected_size = strlen(expected);
	if (size != expected_size)
		return 0;

	/* The same length with different bytes is a different text. */
	if (memcmp(text, expected, size) != 0)
		return 0;

	/* The two texts are equal. */
	return 1;
}

/*
 * Joins the base64 lines between the BEGIN line and the END line into joined.
 *
 * Lines end with LF or CR LF.  Text before the BEGIN line and after the
 * END line is ignored.  A line between them that is empty is EINVAL; any
 * other byte is left for the base64 decoder to judge.
 */
static int
private_join_lines(
	const char *text,
	size_t size,
	struct ssh_buf *joined)
{
	size_t position;
	size_t line_end;
	size_t line_size;
	size_t next;
	int found_begin;
	int found_end;

	position = 0;
	found_begin = 0;
	found_end = 0;

	/* Walks the lines until the END line, joining those after the BEGIN line. */
	while (position < size && !found_end) {
		/* Finds the end of this line and where the next starts. */
		line_end = position;
		while (line_end < size && text[line_end] != '\n')
			line_end++;
		next = line_end;
		if (next < size)
			next++;

		/* A CR before the LF belongs to the line ending. */
		line_size = line_end - position;
		if (line_size > 0 && text[line_end - 1U] == '\r')
			line_size--;

		if (!found_begin) {
			/* Text before the BEGIN line is ignored. */
			if (key_text_is((const uint8_t *)text + position, line_size, SSH_KEY_PRIVATE_BEGIN))
				found_begin = 1;
		} else if (key_text_is((const uint8_t *)text + position, line_size, SSH_KEY_PRIVATE_END)) {
			/* The END line closes the armour. */
			found_end = 1;
		} else {
			/* A base64 line: an empty one is not part of the format. */
			if (line_size == 0)
				return EINVAL;
			ssh_buf_put_bytes(joined, text + position, line_size);
			if (ssh_buf_error(joined) != 0)
				return ssh_buf_error(joined);
		}

		position = next;
	}

	/* An armour that is not closed is malformed. */
	if (!found_begin || !found_end)
		return EINVAL;

	/* Succeeded: the base64 is in joined. */
	return 0;
}

/*
 * Checks the decoded bytes of an openssh-key-v1 file and fills key from them.
 *
 * Every length is checked against what remains, the cipher and KDF must be
 * "none", the checkint pair must agree, the padding must be 1, 2, 3, ...,
 * and the three copies of the public key and the seed must agree.  Nothing
 * is written to key until the end.
 */
static int
private_parse_decoded(
	const uint8_t *data,
	size_t size,
	struct ssh_key *key)
{
	struct ssh_reader outer;
	struct ssh_reader inner;
	const uint8_t *cipher;
	const uint8_t *kdf;
	const uint8_t *options;
	const uint8_t *section;
	const uint8_t *blob;
	const uint8_t *type;
	const uint8_t *private_public;
	const uint8_t *pair;
	const uint8_t *comment;
	const uint8_t *padding;
	size_t cipher_size;
	size_t kdf_size;
	size_t options_size;
	size_t section_size;
	size_t blob_size;
	size_t type_size;
	size_t private_public_size;
	size_t pair_size;
	size_t comment_size;
	size_t padding_size;
	size_t index;
	uint32_t key_count;
	uint32_t checkint_first;
	uint32_t checkint_second;
	uint8_t blob_public[SSH_ED25519_PUBLIC];
	uint8_t derived_public[SSH_ED25519_PUBLIC];
	int error;

	/* Checks the magic and its NUL (PROTOCOL.key 1). */
	if (size < sizeof(private_magic))
		return EINVAL;
	if (memcmp(data, private_magic, sizeof(private_magic)) != 0)
		return EINVAL;

	/* Reads the cipher, the KDF, its options and the key count. */
	ssh_reader_init(&outer, data + sizeof(private_magic), size - sizeof(private_magic));
	ssh_reader_get_string(&outer, &cipher, &cipher_size);
	ssh_reader_get_string(&outer, &kdf, &kdf_size);
	ssh_reader_get_string(&outer, &options, &options_size);
	key_count = ssh_reader_get_u32(&outer);
	error = ssh_reader_error(&outer);
	if (error != 0)
		return EINVAL;

	/* Only an unencrypted file is supported (PROTOCOL.key 5); a passphrase-protected one is refused. */
	if (!key_text_is(cipher, cipher_size, "none"))
		return ENOTSUP;
	if (!key_text_is(kdf, kdf_size, "none"))
		return ENOTSUP;

	/* The options of the KDF "none" are the empty string (PROTOCOL.key 5). */
	if (options_size != 0)
		return EINVAL;

	/* Only a file of one key is supported. */
	if (key_count != 1U)
		return ENOTSUP;

	/* Reads the public key blob and the private section, which must end the file. */
	ssh_reader_get_string(&outer, &blob, &blob_size);
	ssh_reader_get_string(&outer, &section, &section_size);
	error = ssh_reader_finish(&outer);
	if (error != 0)
		return EINVAL;

	/* The public key blob must be an ssh-ed25519 key; another type is not this key. */
	error = ssh_key_parse_public_blob(blob, blob_size, blob_public);
	if (error != 0)
		return error;

	/* The private section is padded to the cipher block size (PROTOCOL.key 3). */
	if (section_size == 0 || section_size % KEY_PRIVATE_BLOCK != 0)
		return EINVAL;

	/* Reads the two check integers, which must be equal. */
	ssh_reader_init(&inner, section, section_size);
	checkint_first = ssh_reader_get_u32(&inner);
	checkint_second = ssh_reader_get_u32(&inner);
	error = ssh_reader_error(&inner);
	if (error != 0)
		return EINVAL;
	if (checkint_first != checkint_second)
		return EINVAL;

	/* Reads the private key in the agent encoding (draft-ietf-sshm-ssh-agent 3.2.3) and the comment. */
	ssh_reader_get_string(&inner, &type, &type_size);
	ssh_reader_get_string(&inner, &private_public, &private_public_size);
	ssh_reader_get_string(&inner, &pair, &pair_size);
	ssh_reader_get_string(&inner, &comment, &comment_size);
	error = ssh_reader_error(&inner);
	if (error != 0)
		return EINVAL;

	/* The key type in the private section must be ssh-ed25519 like the blob's. */
	if (!key_text_is(type, type_size, SSH_KEY_NAME))
		return EINVAL;

	/* The public key is 32 octets, and the pair is the 32-octet seed followed by the public key. */
	if (private_public_size != SSH_ED25519_PUBLIC)
		return EINVAL;
	if (pair_size != KEY_PRIVATE_PAIR_SIZE)
		return EINVAL;

	/* The padding is what remains: fewer bytes than a block, counting 1, 2, 3, ... */
	padding_size = ssh_reader_left(&inner);
	if (padding_size >= KEY_PRIVATE_BLOCK)
		return EINVAL;
	if (padding_size > 0) {
		padding = ssh_reader_get_bytes(&inner, padding_size);
		error = ssh_reader_error(&inner);
		if (error != 0)
			return EINVAL;

		/* Each padding byte is its place counted from 1. */
		for (index = 0; index < padding_size; index++) {
			if (padding[index] != (uint8_t)(index + 1U))
				return EINVAL;
		}
	}

	/* The public key in the blob, in the private key and in the pair must be one. */
	if (!ssh_ct_equal(blob_public, private_public, SSH_ED25519_PUBLIC))
		return EINVAL;
	if (!ssh_ct_equal(private_public, pair + SSH_ED25519_SEED, SSH_ED25519_PUBLIC))
		return EINVAL;

	/* It must also be the public key of the seed (RFC 8032 5.1.5). */
	error = ssh_ed25519_public(pair, derived_public);
	if (error != 0)
		return EIO;
	if (!ssh_ct_equal(derived_public, blob_public, SSH_ED25519_PUBLIC))
		return EINVAL;

	/* A comment with a NUL inside cannot be kept as a C string. */
	for (index = 0; index < comment_size; index++) {
		if (comment[index] == 0)
			return EINVAL;
	}

	/* Fills the result: the seed, the public key and the comment. */
	memcpy(key->seed, pair, SSH_ED25519_SEED);
	memcpy(key->public_key, blob_public, SSH_ED25519_PUBLIC);
	key_copy_comment(key, (const char *)comment, comment_size);
	key->private = 1;

	/* Succeeded: key holds the private key of the file. */
	return 0;
}

/*
 * Builds the private section of a key file (PROTOCOL.key 3, the agent
 * encoding of draft-ietf-sshm-ssh-agent 3.2.3) into inner, padded with
 * 1, 2, 3, ... to a multiple of the block size.
 */
static int
private_put_inner(
	const struct ssh_key *key,
	size_t comment_size,
	uint32_t checkint,
	struct ssh_buf *inner)
{
	uint8_t pair[KEY_PRIVATE_PAIR_SIZE];
	size_t padding_size;
	size_t index;
	int error;

	/* Writes the check integer twice. */
	ssh_buf_put_u32(inner, checkint);
	ssh_buf_put_u32(inner, checkint);

	/* Writes the key type, the public key, and the seed followed by the public key. */
	ssh_buf_put_string(inner, SSH_KEY_NAME, KEY_NAME_LENGTH);
	ssh_buf_put_string(inner, key->public_key, SSH_ED25519_PUBLIC);
	memcpy(pair, key->seed, SSH_ED25519_SEED);
	memcpy(pair + SSH_ED25519_SEED, key->public_key, SSH_ED25519_PUBLIC);
	ssh_buf_put_string(inner, pair, sizeof(pair));
	ssh_wipe(pair, sizeof(pair));

	/* Writes the comment. */
	ssh_buf_put_string(inner, key->comment, comment_size);

	/* Pads to a multiple of the block size with the bytes 1, 2, 3, ... */
	padding_size = (KEY_PRIVATE_BLOCK - inner->size % KEY_PRIVATE_BLOCK) % KEY_PRIVATE_BLOCK;
	for (index = 1; index <= padding_size; index++)
		ssh_buf_put_u8(inner, (uint8_t)index);

	/* Reports a write that failed. */
	error = ssh_buf_error(inner);
	if (error != 0)
		return error;

	/* Succeeded: the private section is built. */
	return 0;
}

/*
 * Measures the comment of key, which must end with a NUL inside its array.
 */
static int
private_comment_size(
	const struct ssh_key *key,
	size_t *comment_size)
{
	size_t index;

	/* Looks for the NUL that ends the comment. */
	for (index = 0; index < SSH_KEY_COMMENT_MAX; index++) {
		if (key->comment[index] == '\0') {
			*comment_size = index;
			return 0;
		}
	}

	/* A comment with no end is not a valid string. */
	return EINVAL;
}
