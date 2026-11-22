/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ssh-ed25519 keys (RFC 8709): the public key and signature encodings of
 * the protocol, the one-line public key form of .pub files and
 * authorized_keys, OpenSSH's private key file (openssh-key-v1, OpenSSH
 * PROTOCOL.key: in the clear, or encrypted with aes256-ctr and the bcrypt
 * KDF as ssh-keygen writes it with a passphrase), and the SHA256
 * fingerprint.
 *
 * No other key type is supported: a blob, a line or a file that names
 * another type is ENOTSUP, which the callers treat as "not this key"
 * (a host key line of another type is skipped, not an error).
 *
 * The seed is secret.  struct ssh_key holding a seed is wiped by
 * ssh_key_clear.  Parsing works on text and bytes the caller read; this
 * part opens no file.
 */

#ifndef SSH_SSH_KEY_H
#define SSH_SSH_KEY_H

#include "ssh-buf.h"
#include "ssh-crypto.h"

/* The key and signature algorithm name (RFC 8709 4 and 6). */
#define SSH_KEY_NAME		"ssh-ed25519"

/* The size of the public key blob: string "ssh-ed25519" (4 + 11) and string of 32 bytes (4 + 32). */
#define SSH_KEY_BLOB_SIZE	51U

/* The size of the signature blob: string "ssh-ed25519" and string of 64 bytes. */
#define SSH_KEY_SIGNATURE_BLOB_SIZE	83U

/* The longest comment kept with a key (longer comments are cut at a character boundary of ASCII; other bytes are kept as they are). */
#define SSH_KEY_COMMENT_MAX	256U

/* The longest private key file accepted, in bytes. */
#define SSH_KEY_FILE_MAX	16384U

/* The longest SHA256 fingerprint text with its NUL ("SHA256:" and 43 characters). */
#define SSH_KEY_FINGERPRINT_SIZE	51U

/* The armour lines of a private key file (OpenSSH writes exactly these). */
#define SSH_KEY_PRIVATE_BEGIN	"-----BEGIN OPENSSH PRIVATE KEY-----"
#define SSH_KEY_PRIVATE_END	"-----END OPENSSH PRIVATE KEY-----"

/* The width of the base64 lines of a private key file as written (OpenSSH wraps at 70). */
#define SSH_KEY_PRIVATE_WRAP	70U

/* The cipher and the KDF of the encrypted private key files read (PROTOCOL.key 2; what ssh-keygen -N writes). */
#define SSH_KEY_CIPHER		"aes256-ctr"
#define SSH_KEY_KDF		"bcrypt"

/* The longest passphrase taken, in bytes. */
#define SSH_KEY_PASSPHRASE_MAX	1024U

/*
 * The most bcrypt rounds an encrypted file may ask for (ssh-keygen -a;
 * OpenSSH writes 16 or 24 by default).  Each round costs a few
 * milliseconds, so the limit keeps a file from holding the caller for
 * more than about a minute.
 */
#define SSH_KEY_ROUNDS_MAX	16384U

/*
 * One ssh-ed25519 key: always the public half, and the seed when private
 * is 1.
 *
 * comment is NUL-terminated (empty when there is none).  Owns no memory;
 * the seed is wiped by ssh_key_clear.
 */
struct ssh_key {
	uint8_t public_key[SSH_ED25519_PUBLIC];
	uint8_t seed[SSH_ED25519_SEED];
	int private;
	char comment[SSH_KEY_COMMENT_MAX];
};

/* Wipes the seed and clears the whole structure. */
void ssh_key_clear(struct ssh_key *key);

/*
 * Makes a new key: a 32-byte seed from ssh_random and its public key
 * (ssh_ed25519_public).  comment (NULL for none) is copied, cut to fit.
 *
 * Returns 0 or EIO (key cleared).
 */
int ssh_key_generate(struct ssh_key *key, const char *comment);

/*
 * Appends the public key blob (RFC 8709 4: string "ssh-ed25519", string
 * of the 32-byte key) to out.  Errors are recorded in out.
 */
void ssh_key_put_public_blob(const uint8_t *public_key, struct ssh_buf *out);

/*
 * Reads a public key blob of size bytes and writes the 32-byte key.
 *
 * Another algorithm name is ENOTSUP; a malformed blob, a key string that
 * is not 32 bytes, or trailing bytes are EINVAL.  Returns 0, EINVAL or
 * ENOTSUP; on an error public_key is not written.
 */
int ssh_key_parse_public_blob(const uint8_t *blob, size_t size, uint8_t *public_key);

/*
 * Signs size bytes at data with key (which must be private; EINVAL
 * otherwise) and appends the signature blob (RFC 8709 6: string
 * "ssh-ed25519", string of the 64-byte signature) to out.
 *
 * Returns 0, EINVAL or EIO (nothing appended on an error).
 */
int ssh_key_sign(const struct ssh_key *key, const uint8_t *data, size_t size, struct ssh_buf *out);

/*
 * Verifies a signature blob of blob_size bytes over size bytes at data
 * with a 32-byte public key.
 *
 * A blob that names another algorithm is ENOTSUP; a malformed blob, a
 * signature string that is not 64 bytes, or trailing bytes are EINVAL
 * (RFC 8709 6); a signature that does not verify is EBADMSG.  Returns 0,
 * EINVAL, ENOTSUP, EBADMSG or EIO.
 */
int ssh_key_verify(const uint8_t *public_key, const uint8_t *data, size_t size, const uint8_t *blob, size_t blob_size);

/*
 * Appends the one-line public key form "ssh-ed25519 <base64 of the blob>
 * <comment>\n" to out (the comment and its space left out when empty), as
 * a .pub file and an authorized_keys line hold it (sshd(8) AUTHORIZED_KEYS
 * FILE FORMAT).  Errors are recorded in out.
 */
void ssh_key_put_public_line(const uint8_t *public_key, const char *comment, struct ssh_buf *out);

/*
 * Reads a public key in the one-line form from size bytes at text (one
 * line, without its LF): the type, one or more spaces or tabs, the
 * base64 blob, and optionally white space and a comment.
 *
 * The type field must be "ssh-ed25519" (another is ENOTSUP), the blob
 * must decode (ssh_base64_decode) and parse (ssh_key_parse_public_blob)
 * and name the same type (EINVAL otherwise).  key receives the public key
 * with private 0 and the comment (cut to fit).  Returns 0, EINVAL or
 * ENOTSUP.
 */
int ssh_key_parse_public_line(const char *text, size_t size, struct ssh_key *key);

/*
 * Reads an OpenSSH private key file of size bytes at text.
 *
 * The text is the BEGIN line, base64 lines, and the END line (lines end
 * with LF or CR LF; text before the BEGIN line and after the END line is
 * ignored).  The decoded bytes (OpenSSH PROTOCOL.key 1, 3, 5) must be:
 * the magic "openssh-key-v1" with its NUL; ciphername "none" and kdfname
 * "none" (another cipher or KDF, an encrypted key, is ENOTSUP) and empty
 * kdfoptions; exactly one key (another count is ENOTSUP); its public
 * blob, which must be ssh-ed25519; then the private section: two equal
 * checkint values (unequal is EINVAL), the private key in the agent
 * encoding (draft-ietf-sshm-ssh-agent-16 3.2.3 for Ed25519: string
 * "ssh-ed25519", string of the 32-byte public key, string of 64 bytes that
 * are the seed followed by the public key again), the comment, and the
 * padding bytes 1, 2, 3, ... up to a multiple of 8 (wrong padding is
 * EINVAL).  The public key must be the same in the blob, in the private
 * key and in the second half of the 64 bytes, and must be the public key
 * of the seed (ssh_ed25519_public); a difference is EINVAL.
 *
 * size above SSH_KEY_FILE_MAX is EMSGSIZE.  key receives the key with
 * private 1 and the comment.  Returns 0, EINVAL, ENOTSUP, EMSGSIZE, ENOMEM
 * or EIO; on an error key is cleared and every decoded byte is wiped.
 */
int ssh_key_parse_private(const char *text, size_t size, struct ssh_key *key);

/*
 * Reads the public half of an OpenSSH private key file of size bytes at
 * text, encrypted or not, without a passphrase: the public key blob,
 * which an encrypted file keeps in the clear (PROTOCOL.key 1).
 *
 * The armour, the magic, the key count and the blob are checked as
 * ssh_key_parse_private checks them; the cipher and the KDF must be
 * "none" and "none" (*encrypted 0) or SSH_KEY_CIPHER and SSH_KEY_KDF with
 * well-formed options (*encrypted 1); another cipher or KDF is ENOTSUP.
 * The private section is not read.  key receives the public key with
 * private 0 and an empty comment.  Returns 0, EINVAL, ENOTSUP, EMSGSIZE or
 * ENOMEM; on an error key is cleared and *encrypted is 0.
 */
int ssh_key_parse_private_public(const char *text, size_t size, struct ssh_key *key, int *encrypted);

/*
 * Reads an OpenSSH private key file of size bytes at text as
 * ssh_key_parse_private does, decrypting an encrypted one with the
 * passphrase (passphrase_size bytes, not NUL-terminated, 1 to
 * SSH_KEY_PASSPHRASE_MAX; EINVAL otherwise): bcrypt_pbkdf with the
 * file's salt and rounds (at most SSH_KEY_ROUNDS_MAX; more is ENOTSUP)
 * gives the 32-byte AES-256 key and the 16-byte first counter block, and
 * the private section is decrypted with aes256-ctr (PROTOCOL.key 2, 3).
 * The decrypted section is checked as an unencrypted one, padded to 16
 * bytes; check integers that differ after the decryption mean the wrong
 * passphrase (EACCES).  A file in the clear is read without the
 * passphrase.
 *
 * The time of an encrypted file grows with its rounds (a fraction of a
 * second for OpenSSH's default).  The passphrase is not kept; the derived
 * key, the counter block and every decrypted byte are wiped.  Returns 0,
 * EACCES, EINVAL, ENOTSUP, EMSGSIZE, ENOMEM or EIO; on an error key is
 * cleared.
 */
int ssh_key_parse_private_passphrase(const char *text, size_t size, const uint8_t *passphrase, size_t passphrase_size, struct ssh_key *key);

/*
 * Appends key (private 1; EINVAL otherwise) to out as an OpenSSH private
 * key file in the layout ssh_key_parse_private reads, unencrypted,
 * checkint the given value twice, base64 wrapped at SSH_KEY_PRIVATE_WRAP
 * characters, each line ending with LF.
 *
 * out should be secret.  Returns 0, EINVAL or the error recorded in out.
 */
int ssh_key_put_private(const struct ssh_key *key, uint32_t checkint, struct ssh_buf *out);

/*
 * Appends key (private 1; EINVAL otherwise) to out as an OpenSSH private
 * key file encrypted with a passphrase, in the layout
 * ssh_key_parse_private_passphrase reads (what ssh-keygen -N writes):
 * cipher SSH_KEY_CIPHER and KDF SSH_KEY_KDF with the salt (1 to
 * SSH_BCRYPT_SALT_MAX bytes) and rounds (1 to SSH_KEY_ROUNDS_MAX) given,
 * the private section padded to the cipher's block and encrypted with
 * the key and the first counter block bcrypt_pbkdf derives from the
 * passphrase (passphrase_size bytes, 1 to SSH_KEY_PASSPHRASE_MAX),
 * checkint the given value twice, wrapped as ssh_key_put_private wraps.
 *
 * The passphrase is not kept; the derived bytes and every byte of the
 * section in the clear are wiped.  out should be secret.  Returns 0,
 * EINVAL, ENOTSUP (rounds above the limit), EIO or the error recorded in
 * out.
 */
int ssh_key_put_private_passphrase(const struct ssh_key *key, uint32_t checkint, const uint8_t *passphrase, size_t passphrase_size, const uint8_t *salt, size_t salt_size, unsigned rounds, struct ssh_buf *out);

/*
 * Writes the SHA256 fingerprint of a public key: "SHA256:" followed by
 * the base64 of SHA-256 of the public key blob without '=' padding, as
 * ssh-keygen -l and ssh print it, and a NUL.
 *
 * capacity is at least SSH_KEY_FINGERPRINT_SIZE (ENOBUFS otherwise).
 * Returns 0, ENOBUFS or EIO.
 */
int ssh_key_fingerprint(const uint8_t *public_key, char *out, size_t capacity);

#endif
