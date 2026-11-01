/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The packet cipher chacha20-poly1305@openssh.com.
 *
 * Written from sections 3, 4, 6 and 7 of draft-ietf-sshm-chacha20-poly1305-04
 * and checked against its Appendix A.  The ChaCha20 stream and the Poly1305
 * tag come from ssh-crypto.c; this file only arranges them: which key and
 * which block counter encrypt which bytes, and what the tag covers.
 */

#include <string.h>

#include "ssh-cipher.h"

/*
 * The block counter of the ChaCha20 block that encrypts the 4-byte length
 * (under K_2) and whose first 32 bytes are the Poly1305 key (under K_1),
 * section 6.
 */
#define CIPHER_COUNTER_FIRST	0U

/* The block counter at which the packet body's keystream starts (section 6). */
#define CIPHER_COUNTER_BODY	1U

/* The size of the encrypted packet_length field (RFC 4253 6). */
#define CIPHER_LENGTH_SIZE	4U

/* The smallest packet the functions take: the length and one more byte. */
#define CIPHER_SIZE_MIN		5U

static void cipher_nonce(uint32_t seq, uint8_t *nonce);
static int cipher_check_size(size_t size);
static int cipher_tag(const struct ssh_chachapoly *cipher, const uint8_t *nonce, const uint8_t *packet, size_t size, uint8_t *tag);
static int cipher_crypt(const struct ssh_chachapoly *cipher, const uint8_t *nonce, uint8_t *packet, size_t size);

/*
 * Takes the 64 bytes of key material (copied; the caller may wipe them).
 *
 * The first 32 bytes are K_1 and the last 32 bytes are K_2 (section 6).
 */
void
ssh_chachapoly_init(
	struct ssh_chachapoly *cipher,
	const uint8_t *key)
{
	/* Copies the first half, which encrypts the body and keys the tag. */
	memcpy(cipher->main_key, key, SSH_CHACHA20_KEY);

	/* Copies the second half, which encrypts the length. */
	memcpy(cipher->header_key, key + SSH_CHACHA20_KEY, SSH_CHACHA20_KEY);
}

/*
 * Wipes the keys.
 */
void
ssh_chachapoly_clear(
	struct ssh_chachapoly *cipher)
{
	/* Wipes the key that encrypts the body and keys the tag. */
	ssh_wipe(cipher->main_key, sizeof(cipher->main_key));

	/* Wipes the key that encrypts the length. */
	ssh_wipe(cipher->header_key, sizeof(cipher->header_key));
}

/*
 * Decrypts the 4-byte encrypted packet_length at encrypted for sequence
 * number seq (K_2, block counter 0) and writes the length to *length.
 *
 * The length is not checked here.
 */
int
ssh_chachapoly_length(
	const struct ssh_chachapoly *cipher,
	uint32_t seq,
	const uint8_t *encrypted,
	uint32_t *length)
{
	uint8_t nonce[SSH_CHACHA20_NONCE];
	uint8_t plain[CIPHER_LENGTH_SIZE];
	int error;

	/* Builds the nonce, which is the sequence number as a 64-bit big-endian integer. */
	cipher_nonce(seq, nonce);

	/* Decrypts the 4 bytes with the length key at block counter 0. */
	error = ssh_chacha20_xor(cipher->header_key, CIPHER_COUNTER_FIRST, nonce, encrypted, plain, CIPHER_LENGTH_SIZE);
	if (error != 0) {
		*length = 0;
		return error;
	}

	/* Reads the plaintext length, which is a big-endian integer. */
	*length = ssh_load_be32(plain);
	ssh_wipe(plain, sizeof(plain));

	/* Succeeded: *length holds the decrypted packet_length. */
	return 0;
}

/*
 * Encrypts a packet in place and writes its tag.
 *
 * The tag is computed over the encrypted bytes, so the bytes are
 * encrypted first and authenticated second.
 */
int
ssh_chachapoly_seal(
	const struct ssh_chachapoly *cipher,
	uint32_t seq,
	uint8_t *packet,
	size_t size,
	uint8_t *tag)
{
	uint8_t nonce[SSH_CHACHA20_NONCE];
	int error;

	/* Refuses a size no packet has. */
	error = cipher_check_size(size);
	if (error != 0)
		return error;

	/* Builds the nonce, which is the sequence number as a 64-bit big-endian integer. */
	cipher_nonce(seq, nonce);

	/* Encrypts the length with K_2 and the rest with K_1. */
	error = cipher_crypt(cipher, nonce, packet, size);
	if (error != 0) {
		/* Leaves no plaintext in the packet or the tag for a caller that ignores the error. */
		ssh_wipe(packet, size);
		ssh_wipe(tag, SSH_CHACHAPOLY_TAG);
		return error;
	}

	/* Authenticates all the encrypted bytes. */
	error = cipher_tag(cipher, nonce, packet, size, tag);
	if (error != 0) {
		ssh_wipe(packet, size);
		ssh_wipe(tag, SSH_CHACHAPOLY_TAG);
		return error;
	}

	/* Succeeded: the packet is encrypted and the tag written. */
	return 0;
}

/*
 * Checks a received packet's tag and, when it is right, decrypts the
 * packet in place.
 *
 * Nothing of the packet is decrypted before the tag is known to be right
 * (section 7), and a wrong tag leaves the packet as it was.
 */
int
ssh_chachapoly_open(
	const struct ssh_chachapoly *cipher,
	uint32_t seq,
	uint8_t *packet,
	size_t size,
	const uint8_t *tag)
{
	uint8_t nonce[SSH_CHACHA20_NONCE];
	uint8_t expected[SSH_CHACHAPOLY_TAG];
	int equal;
	int error;

	/* Refuses a size no packet has. */
	error = cipher_check_size(size);
	if (error != 0)
		return error;

	/* Builds the nonce, which is the sequence number as a 64-bit big-endian integer. */
	cipher_nonce(seq, nonce);

	/* Computes the tag the encrypted bytes should carry. */
	error = cipher_tag(cipher, nonce, packet, size, expected);
	if (error != 0)
		return error;

	/* Compares the two tags in a time that does not depend on where they differ. */
	equal = ssh_ct_equal(expected, tag, SSH_CHACHAPOLY_TAG);
	ssh_wipe(expected, sizeof(expected));
	if (equal == 0)
		return EBADMSG;

	/* Decrypts the length with K_2 and the rest with K_1, now that the bytes are known to be genuine. */
	error = cipher_crypt(cipher, nonce, packet, size);
	if (error != 0) {
		/* Leaves nothing half decrypted behind. */
		ssh_wipe(packet, size);
		return error;
	}

	/* Succeeded: the tag was right and the packet is decrypted. */
	return 0;
}

/*
 * Writes the ChaCha20 nonce of a packet: its sequence number as a 64-bit
 * big-endian integer (section 6).
 */
static void
cipher_nonce(
	uint32_t seq,
	uint8_t *nonce)
{
	/* The upper four bytes are zero because the sequence number has 32 bits. */
	ssh_store_be64(nonce, (uint64_t)seq);
}

/*
 * Checks that size can be a packet: the length field and at least one
 * more byte, and a packet_length no larger than the packet layer accepts.
 */
static int
cipher_check_size(
	size_t size)
{
	/* Refuses a packet without a byte after its length. */
	if (size < CIPHER_SIZE_MIN)
		return EINVAL;

	/* Refuses a packet whose packet_length would pass the limit of the packet layer. */
	if (size - CIPHER_LENGTH_SIZE > SSH_PACKET_LIMIT)
		return EINVAL;

	/* Accepts the size. */
	return 0;
}

/*
 * Computes the Poly1305 tag of the encrypted packet.
 *
 * The one-time key is the first 32 bytes of the keystream of K_1 at block
 * counter 0 (section 6): the keystream XORed with zeros.
 */
static int
cipher_tag(
	const struct ssh_chachapoly *cipher,
	const uint8_t *nonce,
	const uint8_t *packet,
	size_t size,
	uint8_t *tag)
{
	uint8_t zeros[SSH_POLY1305_KEY];
	uint8_t poly_key[SSH_POLY1305_KEY];
	int error;

	/* Prepares the zero bytes whose XOR with the keystream is the keystream. */
	memset(zeros, 0, sizeof(zeros));

	/* Takes the Poly1305 key from block 0 of the body cipher. */
	error = ssh_chacha20_xor(cipher->main_key, CIPHER_COUNTER_FIRST, nonce, zeros, poly_key, sizeof(poly_key));
	if (error != 0) {
		ssh_wipe(poly_key, sizeof(poly_key));
		return error;
	}

	/* Authenticates the encrypted length followed by the encrypted rest. */
	error = ssh_poly1305(poly_key, packet, size, tag);
	ssh_wipe(poly_key, sizeof(poly_key));
	if (error != 0)
		return error;

	/* Succeeded: tag holds the Poly1305 tag. */
	return 0;
}

/*
 * Applies the keystreams to a packet in place: K_2 at block counter 0 for
 * the 4-byte length, K_1 from block counter 1 for the rest (section 6).
 * Encrypting and decrypting are the same operation.
 */
static int
cipher_crypt(
	const struct ssh_chachapoly *cipher,
	const uint8_t *nonce,
	uint8_t *packet,
	size_t size)
{
	int error;

	/* Applies the length key to the first four bytes. */
	error = ssh_chacha20_xor(cipher->header_key, CIPHER_COUNTER_FIRST, nonce, packet, packet, CIPHER_LENGTH_SIZE);
	if (error != 0)
		return error;

	/* Applies the body key from block counter 1 to everything after the length. */
	error = ssh_chacha20_xor(cipher->main_key, CIPHER_COUNTER_BODY, nonce, packet + CIPHER_LENGTH_SIZE, packet + CIPHER_LENGTH_SIZE, size - CIPHER_LENGTH_SIZE);
	if (error != 0)
		return error;

	/* Succeeded: the packet is transformed. */
	return 0;
}
