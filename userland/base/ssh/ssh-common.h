/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The conventions every part of ssh, sshd and ssh-keygen shares, and the
 * small helpers they all use: byte order, name-lists, base64.
 *
 * The programs speak the SSH protocol version 2 (RFC 4251 to RFC 4254) as
 * OpenSSH speaks it, with one algorithm of each kind: the key exchange
 * mlkem768x25519-sha256, the host and user key ssh-ed25519 (RFC 8709),
 * the cipher chacha20-poly1305@openssh.com, no MAC of its own and no
 * compression.  The cryptographic primitives come from the system's
 * crypto library through ssh-crypto.h and from nowhere else.
 *
 * Untrusted input.  Every byte that comes from the network, from a key
 * file, from an authorized_keys or known_hosts file, from a configuration
 * file or from the other process of a privilege-separated pair is
 * untrusted.  A function that reads untrusted bytes never reads or writes
 * outside its buffers, never indexes an array with a value it has not
 * checked against the array, never loops without a bound that the buffer
 * sizes or a limit below give, and returns an error rather than asserting.
 *
 * Arithmetic never relies on undefined or implementation-defined
 * behaviour: no signed overflow (a sum or a product that could leave its
 * type is computed in a wider unsigned type, or checked against the limit
 * first), no shift of a negative value, no shift by the width of the type
 * or more, and a value is checked before it is stored in a narrower type.
 *
 * Secrets.  A secret is a private key or seed, a shared secret, the
 * exchange hash before it is public, a derived key, a password, and a
 * plaintext packet.  Code that handles a secret compares it with
 * ssh_ct_equal (ssh-crypto.h), never with memcmp, and wipes it with
 * ssh_wipe before the memory is freed, reused or goes out of scope, on
 * every path.  A structure that holds a secret has a _clear or _free
 * function that wipes it.
 *
 * The error values, used alike by every part (errno values; 0 is
 * success):
 *
 *   EINVAL     Malformed input: an encoding that breaks its
 *              specification, a length or a value out of its range; or a
 *              caller's argument out of the range a comment allows.
 *   EPROTO     The peer broke the protocol: a message in the wrong state,
 *              a value it was not offered, a message strict key exchange
 *              forbids.
 *   EBADMSG    An integrity check failed: a packet's tag, a signature.
 *   ENOTSUP    The input is valid but uses something outside what these
 *              programs support (another key type, an encrypted key file,
 *              a feature not provided).
 *   EACCES     A policy refused a valid request: an authentication that
 *              failed, a host key that does not match, a channel type
 *              that is not offered.
 *   EMSGSIZE   A packet, a line or a field is longer than the limit
 *              below allows.
 *   ENOBUFS    A buffer the caller sized is too small, or an output
 *              queue is full (back-pressure: write pending output, then
 *              try again; not an error of the connection).
 *   ENOMEM     An allocation failed.
 *   EOVERFLOW  A counter or a window would wrap.
 *   EAGAIN     More input is needed before anything can be returned.
 *   ECONNABORTED  The peer sent SSH_MSG_DISCONNECT.
 *   EIO        The operating system or the crypto library failed.
 *   ENOSYS     Not implemented (only in the skeleton).
 *
 * Nothing keeps hidden global state; everything lives in structures the
 * caller owns.  The programs are single-threaded.
 *
 * The language is ANSI C (C89) with <stdint.h>; 64-bit integers are
 * uint64_t; a 64-bit constant is built from two 32-bit halves.
 */

#ifndef SSH_SSH_COMMON_H
#define SSH_SSH_COMMON_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The largest packet_length accepted from a peer (RFC 4253 6.1 requires at
 * least 35000; a larger limit is allowed and OpenSSH's own limit is
 * 256 KiB, so a peer that sends a large non-data message is not refused).
 */
#define SSH_PACKET_LIMIT	262144U

/*
 * The largest payload these programs send in one packet, and the largest
 * channel data packet they announce (RFC 4253 6.1's 32768).
 */
#define SSH_PAYLOAD_SEND_MAX	32768U

/*
 * The longest identification line including CR LF (RFC 4253 4.2: 255
 * characters).
 */
#define SSH_VERSION_LINE_MAX	255U

/*
 * The most lines a client accepts before the server's identification
 * line, and the most bytes in all of them (RFC 4253 4.2 lets a server send
 * other lines first; it sets no bound, so one is chosen here).
 */
#define SSH_VERSION_PRELUDE_LINES	64U
#define SSH_VERSION_PRELUDE_BYTES	16384U

/* The longest algorithm name (RFC 4251 6: at most 64 characters). */
#define SSH_NAME_MAX		64U

/* The longest name-list accepted from a peer, in bytes. */
#define SSH_NAMELIST_MAX	8192U

/* The longest user name accepted (in a USERAUTH_REQUEST and in a configuration). */
#define SSH_USER_MAX		64U

/*
 * The identification line these programs send (RFC 4253 4.2), without
 * CR LF.  The software version names the implementation, not a release
 * of OpenSSH, so a peer applies no OpenSSH-specific workaround to it.
 */
#define SSH_VERSION_STRING	"SSH-2.0-zedBSD_1.0"

/* Reads a 32-bit big-endian integer from bytes[0..3]. */
uint32_t ssh_load_be32(const uint8_t *bytes);

/* Reads a 64-bit big-endian integer from bytes[0..7]. */
uint64_t ssh_load_be64(const uint8_t *bytes);

/* Writes value big-endian into bytes[0..3]. */
void ssh_store_be32(uint8_t *bytes, uint32_t value);

/* Writes value big-endian into bytes[0..7]. */
void ssh_store_be64(uint8_t *bytes, uint64_t value);

/*
 * Checks that size bytes at list form a valid name-list (RFC 4251 5):
 * names separated by single commas, each 1 to SSH_NAME_MAX bytes of
 * printable US-ASCII other than a comma (0x21 to 0x7e), no empty name, no
 * leading or trailing comma; an empty list (size 0) is valid.
 *
 * list may be NULL only when size is 0.  Returns 0, EINVAL for a list
 * that breaks the rules, EMSGSIZE when size exceeds SSH_NAMELIST_MAX.
 */
int ssh_namelist_check(const char *list, size_t size);

/*
 * Reports whether the name-list of size bytes at list (already checked
 * with ssh_namelist_check) contains name exactly (case-sensitive, RFC 4251
 * 6).
 *
 * name is a NUL-terminated string.  Returns 1 or 0.
 */
int ssh_namelist_contains(const char *list, size_t size, const char *name);

/*
 * Returns the number of characters of base64 (RFC 4648 4, with '='
 * padding) that encode size bytes, not counting a NUL.  size is at most
 * (SIZE_MAX / 4) * 3 - 3; a larger size returns 0.
 */
size_t ssh_base64_encoded_size(size_t size);

/*
 * Encodes size bytes at in as base64 (RFC 4648 4, alphabet of Table 1,
 * '=' padding) into out, followed by a NUL.
 *
 * capacity is the room at out including the NUL; when it is smaller
 * than ssh_base64_encoded_size(size) + 1, ENOBUFS is returned and out is
 * not written.  pad is 1 for '=' padding, 0 to leave it out (OpenSSH's
 * SHA256 fingerprint form).  in may be NULL only when size is 0.
 * Returns 0 or ENOBUFS.
 */
int ssh_base64_encode(const uint8_t *in, size_t size, char *out, size_t capacity, int pad);

/*
 * Decodes size characters of base64 at in (RFC 4648 4) into out.
 *
 * Only the 64 characters of the alphabet and '=' padding at the end are
 * accepted (no white space, no line breaks: callers that read wrapped
 * text join the lines first).  The padding is required when the length
 * is not a multiple of 4 in the encoded form OpenSSH writes; an input
 * whose length is not a multiple of 4 is EINVAL.  Non-zero bits in the
 * unused low bits of the last character are EINVAL (RFC 4648 3.5, so one
 * key has one encoding).  *out_size receives the decoded length.
 * capacity smaller than the decoded length is ENOBUFS (nothing written).
 * Returns 0, EINVAL or ENOBUFS; on an error *out_size is 0.
 */
int ssh_base64_decode(const char *in, size_t size, uint8_t *out, size_t capacity, size_t *out_size);

#endif
