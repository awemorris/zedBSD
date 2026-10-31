/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The packet cipher chacha20-poly1305@openssh.com.
 *
 * Skeleton: every function answers ENOSYS (or does nothing) until it is
 * implemented.
 */

#include "ssh-cipher.h"

/*
 * Takes the 64 bytes of key material (copied; the caller may wipe them).
 */
void
ssh_chachapoly_init(
	struct ssh_chachapoly *cipher,
	const uint8_t *key)
{
	/* Marks the arguments used until the function is implemented. */
	(void)cipher;
	(void)key;
}

/*
 * Wipes the keys.
 */
void
ssh_chachapoly_clear(
	struct ssh_chachapoly *cipher)
{
	/* Marks the arguments used until the function is implemented. */
	(void)cipher;
}

/*
 * Decrypts the 4-byte encrypted packet_length at encrypted for sequence
 * number seq (K_2, block counter 0) and writes the length to *length.
 */
int
ssh_chachapoly_length(
	const struct ssh_chachapoly *cipher,
	uint32_t seq,
	const uint8_t *encrypted,
	uint32_t *length)
{
	/* Marks the arguments used until the function is implemented. */
	(void)cipher;
	(void)seq;
	(void)encrypted;
	(void)length;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Encrypts a packet in place and writes its tag.
 */
int
ssh_chachapoly_seal(
	const struct ssh_chachapoly *cipher,
	uint32_t seq,
	uint8_t *packet,
	size_t size,
	uint8_t *tag)
{
	/* Marks the arguments used until the function is implemented. */
	(void)cipher;
	(void)seq;
	(void)packet;
	(void)size;
	(void)tag;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Checks a received packet's tag and, when it is right, decrypts the
 * packet in place.
 */
int
ssh_chachapoly_open(
	const struct ssh_chachapoly *cipher,
	uint32_t seq,
	uint8_t *packet,
	size_t size,
	const uint8_t *tag)
{
	/* Marks the arguments used until the function is implemented. */
	(void)cipher;
	(void)seq;
	(void)packet;
	(void)size;
	(void)tag;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}
