/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ssh-ed25519 keys: blobs, signatures, public key lines, private key files, fingerprints.
 *
 * Skeleton: every function answers ENOSYS (or does nothing) until it is
 * implemented.
 */

#include "ssh-key.h"

/*
 * Wipes the seed and clears the whole structure.
 */
void
ssh_key_clear(
	struct ssh_key *key)
{
	/* Marks the arguments used until the function is implemented. */
	(void)key;
}

/*
 * Makes a new key: a 32-byte seed from ssh_random and its public key
 * (ssh_ed25519_public).
 */
int
ssh_key_generate(
	struct ssh_key *key,
	const char *comment)
{
	/* Marks the arguments used until the function is implemented. */
	(void)key;
	(void)comment;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
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
	/* Marks the arguments used until the function is implemented. */
	(void)public_key;
	(void)out;
}

/*
 * Reads a public key blob of size bytes and writes the 32-byte key.
 */
int
ssh_key_parse_public_blob(
	const uint8_t *blob,
	size_t size,
	uint8_t *public_key)
{
	/* Marks the arguments used until the function is implemented. */
	(void)blob;
	(void)size;
	(void)public_key;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Signs size bytes at data with key (which must be private; EINVAL
 * otherwise) and appends the signature blob (RFC 8709 6: string
 * "ssh-ed25519", string of the 64-byte signature) to out.
 */
int
ssh_key_sign(
	const struct ssh_key *key,
	const uint8_t *data,
	size_t size,
	struct ssh_buf *out)
{
	/* Marks the arguments used until the function is implemented. */
	(void)key;
	(void)data;
	(void)size;
	(void)out;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
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
	/* Marks the arguments used until the function is implemented. */
	(void)public_key;
	(void)data;
	(void)size;
	(void)blob;
	(void)blob_size;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Appends the one-line public key form "ssh-ed25519 <base64 of the blob>
 * <comment>\n" to out (the comment and its space left out when empty),
 * as a .pub file and an authorized_keys line hold it (sshd(8)
 * AUTHORIZED_KEYS FILE FORMAT).
 */
void
ssh_key_put_public_line(
	const uint8_t *public_key,
	const char *comment,
	struct ssh_buf *out)
{
	/* Marks the arguments used until the function is implemented. */
	(void)public_key;
	(void)comment;
	(void)out;
}

/*
 * Reads a public key in the one-line form from size bytes at text (one
 * line, without its LF): the type, one or more spaces or tabs, the
 * base64 blob, and optionally white space and a comment.
 */
int
ssh_key_parse_public_line(
	const char *text,
	size_t size,
	struct ssh_key *key)
{
	/* Marks the arguments used until the function is implemented. */
	(void)text;
	(void)size;
	(void)key;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Reads an OpenSSH private key file of size bytes at text.
 */
int
ssh_key_parse_private(
	const char *text,
	size_t size,
	struct ssh_key *key)
{
	/* Marks the arguments used until the function is implemented. */
	(void)text;
	(void)size;
	(void)key;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Appends key (private 1; EINVAL otherwise) to out as an OpenSSH private
 * key file in the layout ssh_key_parse_private reads, unencrypted,
 * checkint the given value twice, base64 wrapped at SSH_KEY_PRIVATE_WRAP
 * characters, each line ending with LF.
 */
int
ssh_key_put_private(
	const struct ssh_key *key,
	uint32_t checkint,
	struct ssh_buf *out)
{
	/* Marks the arguments used until the function is implemented. */
	(void)key;
	(void)checkint;
	(void)out;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
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
	/* Marks the arguments used until the function is implemented. */
	(void)public_key;
	(void)out;
	(void)capacity;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}
