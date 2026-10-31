/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The shared helpers of ssh, sshd and ssh-keygen: byte order, name-lists, base64.
 *
 * Skeleton: every function answers ENOSYS (or does nothing) until it is
 * implemented.
 */

#include "ssh-common.h"

/*
 * Reads a 32-bit big-endian integer from bytes[0..3].
 */
uint32_t
ssh_load_be32(
	const uint8_t *bytes)
{
	/* Marks the arguments used until the function is implemented. */
	(void)bytes;

	/* Reports that the function is not implemented yet. */
	return 0;
}

/*
 * Reads a 64-bit big-endian integer from bytes[0..7].
 */
uint64_t
ssh_load_be64(
	const uint8_t *bytes)
{
	/* Marks the arguments used until the function is implemented. */
	(void)bytes;

	/* Reports that the function is not implemented yet. */
	return 0;
}

/*
 * Writes value big-endian into bytes[0..3].
 */
void
ssh_store_be32(
	uint8_t *bytes,
	uint32_t value)
{
	/* Marks the arguments used until the function is implemented. */
	(void)bytes;
	(void)value;
}

/*
 * Writes value big-endian into bytes[0..7].
 */
void
ssh_store_be64(
	uint8_t *bytes,
	uint64_t value)
{
	/* Marks the arguments used until the function is implemented. */
	(void)bytes;
	(void)value;
}

/*
 * Checks that size bytes at list form a valid name-list (RFC 4251 5):
 * names separated by single commas, each 1 to SSH_NAME_MAX bytes of
 * printable US-ASCII other than a comma (0x21 to 0x7e), no empty name,
 * no leading or trailing comma; an empty list (size 0) is valid.
 */
int
ssh_namelist_check(
	const char *list,
	size_t size)
{
	/* Marks the arguments used until the function is implemented. */
	(void)list;
	(void)size;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Reports whether the name-list of size bytes at list (already checked
 * with ssh_namelist_check) contains name exactly (case-sensitive, RFC
 * 4251 6).
 */
int
ssh_namelist_contains(
	const char *list,
	size_t size,
	const char *name)
{
	/* Marks the arguments used until the function is implemented. */
	(void)list;
	(void)size;
	(void)name;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Returns the number of characters of base64 (RFC 4648 4, with '='
 * padding) that encode size bytes, not counting a NUL.
 */
size_t
ssh_base64_encoded_size(
	size_t size)
{
	/* Marks the arguments used until the function is implemented. */
	(void)size;

	/* Reports that the function is not implemented yet. */
	return 0;
}

/*
 * Encodes size bytes at in as base64 (RFC 4648 4, alphabet of Table 1,
 * '=' padding) into out, followed by a NUL.
 */
int
ssh_base64_encode(
	const uint8_t *in,
	size_t size,
	char *out,
	size_t capacity,
	int pad)
{
	/* Marks the arguments used until the function is implemented. */
	(void)in;
	(void)size;
	(void)out;
	(void)capacity;
	(void)pad;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Decodes size characters of base64 at in (RFC 4648 4) into out.
 */
int
ssh_base64_decode(
	const char *in,
	size_t size,
	uint8_t *out,
	size_t capacity,
	size_t *out_size)
{
	/* Marks the arguments used until the function is implemented. */
	(void)in;
	(void)size;
	(void)out;
	(void)capacity;
	(void)out_size;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}
