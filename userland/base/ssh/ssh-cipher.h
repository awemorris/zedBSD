/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The packet cipher chacha20-poly1305@openssh.com: the construction of
 * draft-ietf-sshm-chacha20-poly1305-04 (sections 6 and 7), built from the
 * ChaCha20 and Poly1305 of ssh-crypto.h.  It is not RFC 8439's AEAD (no
 * padding of the authenticated data, no lengths block, a 64-bit counter
 * and an 8-byte nonce).
 *
 * For a packet with sequence number seq, the nonce is seq as a 64-bit
 * big-endian integer (section 6).  The 64 bytes of key material form two
 * keys: K_1, the first 32 bytes, encrypts the rest of the packet from
 * block counter 1 and, at block counter 0, gives the Poly1305 key (its
 * first 32 bytes); K_2, the last 32 bytes, encrypts the 4-byte
 * packet_length at block counter 0 (section 6).  The tag is Poly1305 over
 * the encrypted length followed by the encrypted rest (section 6), and is
 * checked in constant time before anything is decrypted (section 7).
 *
 * The keys are secret.  Every function wipes the Poly1305 key and the
 * keystream it made.
 */

#ifndef SSH_SSH_CIPHER_H
#define SSH_SSH_CIPHER_H

#include "ssh-crypto.h"

/* The name of the cipher in SSH_MSG_KEXINIT. */
#define SSH_CIPHER_NAME		"chacha20-poly1305@openssh.com"

/* The key material the cipher takes from the key exchange, in bytes (section 6: 512 bits). */
#define SSH_CHACHAPOLY_KEY	64U

/* The tag appended to every packet, in bytes. */
#define SSH_CHACHAPOLY_TAG	16U

/*
 * The block size the packet layer pads to (RFC 4253 6: at least 8).  With
 * this cipher the 4-byte length is not counted in the alignment: the
 * padding makes padding_length byte + payload + padding a multiple of 8,
 * as OpenSSH does for an authenticated cipher whose length is not
 * encrypted with the rest.
 */
#define SSH_CHACHAPOLY_BLOCK	8U

/*
 * The keys of one direction.
 *
 * main_key is K_1 (bytes 0 to 31 of the key material), header_key K_2
 * (bytes 32 to 63).  Secret; wiped by ssh_chachapoly_clear.
 */
struct ssh_chachapoly {
	uint8_t main_key[SSH_CHACHA20_KEY];
	uint8_t header_key[SSH_CHACHA20_KEY];
};

/* Takes the 64 bytes of key material (copied; the caller may wipe them). */
void ssh_chachapoly_init(struct ssh_chachapoly *cipher, const uint8_t *key);

/* Wipes the keys. */
void ssh_chachapoly_clear(struct ssh_chachapoly *cipher);

/*
 * Decrypts the 4-byte encrypted packet_length at encrypted for sequence
 * number seq (K_2, block counter 0) and writes the length to *length.
 *
 * The length is not checked here (the packet layer checks it against its
 * limits before it waits for the rest).  Returns 0 or EIO.
 */
int ssh_chachapoly_length(const struct ssh_chachapoly *cipher, uint32_t seq, const uint8_t *encrypted, uint32_t *length);

/*
 * Encrypts a packet in place and writes its tag.
 *
 * packet holds size bytes: the 4-byte packet_length (plaintext, big-
 * endian) followed by padding_length, payload and padding; size is at
 * least 5.  The first 4 bytes are encrypted with K_2 at block counter 0,
 * the rest with K_1 from block counter 1, and tag receives Poly1305, keyed
 * by the first 32 bytes of K_1's block 0, over all size encrypted bytes.
 *
 * Returns 0 or EIO (packet and tag then hold no plaintext: the caller
 * drops the connection).
 */
int ssh_chachapoly_seal(const struct ssh_chachapoly *cipher, uint32_t seq, uint8_t *packet, size_t size, uint8_t *tag);

/*
 * Checks a received packet's tag and, when it is right, decrypts the
 * packet in place.
 *
 * packet holds size encrypted bytes (the encrypted length and the rest),
 * size at least 5; tag is the 16 received bytes.  The expected tag is
 * computed over the encrypted bytes and compared with ssh_ct_equal; on a
 * mismatch packet is not changed and EBADMSG is returned.  On a match the
 * first 4 bytes are decrypted with K_2 and the rest with K_1 from block
 * counter 1.
 *
 * Returns 0, EBADMSG or EIO.
 */
int ssh_chachapoly_open(const struct ssh_chachapoly *cipher, uint32_t seq, uint8_t *packet, size_t size, const uint8_t *tag);

#endif
