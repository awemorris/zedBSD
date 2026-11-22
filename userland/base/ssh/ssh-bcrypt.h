/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * bcrypt_pbkdf, the key derivation of OpenSSH's encrypted private key
 * files (kdfname "bcrypt"), and the Blowfish cipher it is built on.
 *
 * Blowfish is a 64-bit block cipher whose initial P-array and S-boxes are
 * the hexadecimal digits of the fractional part of pi; bcrypt's key
 * schedule adds the salted expansion.  bcrypt_pbkdf has PBKDF2's shape,
 * with SHA-512 and a bcrypt-based hash in the place of HMAC.
 * Neither is in the crypto library's interface (OpenSSL has no
 * bcrypt_pbkdf), so this part is plain C on top of ssh_sha512.
 *
 * Not constant time: Blowfish looks up its S-boxes with bytes of its
 * state, which here derives from the passphrase.  Reading every entry for
 * each lookup would make one key file take minutes.  The derivation runs
 * once when a key file is opened, on the user's own machine; what a
 * process sharing the processor's caches could learn from the timing is
 * the risk that remains.
 */

#ifndef SSH_SSH_BCRYPT_H
#define SSH_SSH_BCRYPT_H

#include "ssh-common.h"

/* The words of Blowfish's P-array, and the S-boxes and their entries (Schneier 1993). */
#define SSH_BLOWFISH_P		18U
#define SSH_BLOWFISH_SBOXES	4U
#define SSH_BLOWFISH_ENTRIES	256U

/* The longest key Blowfish's own key schedule takes in bytes (448 bits). */
#define SSH_BLOWFISH_KEY_MAX	56U

/* The size of one bcrypt hash in bytes, and the most bytes bcrypt_pbkdf derives (32 hashes interleaved). */
#define SSH_BCRYPT_HASH		32U
#define SSH_BCRYPT_OUTPUT_MAX	(SSH_BCRYPT_HASH * SSH_BCRYPT_HASH)

/* The longest salt taken in bytes (OpenSSH writes 16). */
#define SSH_BCRYPT_SALT_MAX	1024U

/*
 * The state of Blowfish: the P-array and the four S-boxes.  Secret once a
 * key went in; wiped by ssh_blowfish_clear.  Owns no memory.
 */
struct ssh_blowfish {
	uint32_t p[SSH_BLOWFISH_P];
	uint32_t s[SSH_BLOWFISH_SBOXES][SSH_BLOWFISH_ENTRIES];
};

/*
 * Sets up Blowfish with key (1 to SSH_BLOWFISH_KEY_MAX bytes; EINVAL
 * otherwise): the digits of pi, then the key schedule of Schneier 1993
 * (the key XORed into the P-array, cycled, then the P-array and the
 * S-boxes replaced by the chained encryptions of a zero block).
 * Returns 0 or EINVAL.
 */
int ssh_blowfish_init(struct ssh_blowfish *state, const uint8_t *key, size_t key_size);

/* Encrypts one 64-bit block given as its left and right 32-bit halves (big-endian words of the block). */
void ssh_blowfish_encrypt(const struct ssh_blowfish *state, uint32_t *left, uint32_t *right);

/* Wipes the state. */
void ssh_blowfish_clear(struct ssh_blowfish *state);

/*
 * Derives out_size bytes into out from the passphrase (passphrase_size
 * bytes, not NUL-terminated) and salt with rounds rounds, as OpenBSD's
 * bcrypt_pbkdf(3) does.
 *
 * passphrase_size, salt_size (at most SSH_BCRYPT_SALT_MAX), rounds and
 * out_size (at most SSH_BCRYPT_OUTPUT_MAX) must be at least 1 (EINVAL
 * otherwise; out is not written).  The time grows with rounds.  Every
 * secret in local memory is wiped.  Returns 0, EINVAL, or EIO (the
 * SHA-512 of the crypto library failed; out is wiped).
 */
int ssh_bcrypt_pbkdf(const uint8_t *passphrase, size_t passphrase_size, const uint8_t *salt, size_t salt_size, unsigned rounds, uint8_t *out, size_t out_size);

#endif
