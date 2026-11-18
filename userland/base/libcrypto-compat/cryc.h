/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The conventions shared by every part of libcrypto-compat and
 * libssl-compat, and the small helpers every part uses: constant-time
 * comparison and selection, the wiping of secrets, and the reading and
 * writing of integers in a byte order.
 *
 * libcrypto-compat holds the cryptography (hashes, MACs, AEADs, key
 * exchange, signatures, X.509 and path validation, the random source, the
 * error queue); libssl-compat is a TLS client (TLS 1.3 and TLS 1.2) built
 * on it.  Their public faces are subsets of OpenSSL's libcrypto and
 * libssl interfaces, in <compat/crypto/...> and <compat/ssl/...>
 * (include/libc/compat/).  Everything named cryc_ is libcrypto-compat's
 * internal interface: libcrypto-compat exports it to libssl-compat under
 * a private version node, with no promise to any other program; sslc_
 * names are libssl-compat's own and are not exported.
 *
 * Two forms of build.  On zedBSD (and POSIX hosts) each library is a
 * shared object.  On Windows both are static libraries linked into the
 * program's executable (there is no DLL of these libraries and no
 * DllMain): every external name of the two libraries then shares the
 * namespace of the program's own code.  So every
 * function or object that is not static is either a public name of the
 * subset (in <compat/crypto/...> or <compat/ssl/...>) or starts with
 * cryc_ (libcrypto-compat) or sslc_ (libssl-compat); everything else is
 * static.  No header carries an export, import or visibility attribute:
 * what a shared object exports is set by its version script alone.
 *
 * Untrusted input.  Every byte that comes from the network, from a
 * certificate file, or from a caller of the public interface is untrusted.
 * A function that reads untrusted bytes never reads or writes outside its
 * buffers, never indexes an array with a value it has not checked against
 * the array, never loops without a bound that the buffer sizes or a limit
 * below give, and returns an error rather than asserting.
 *
 * Arithmetic never relies on undefined or implementation-defined
 * behaviour: no signed overflow (a sum or a product that could leave its
 * type is computed in a wider unsigned type), no shift of a negative value
 * and no shift by the width of the type or more, and a value is reduced
 * or checked explicitly before it is stored in a narrower type.
 *
 * Secrets.  A secret is a private key, a shared secret, a key-schedule
 * secret, a traffic key or IV, a MAC key, the internal state of a hash
 * that has absorbed a secret, a plaintext before it is encrypted or after
 * it is decrypted, and the PRNG output that becomes any of these.  Code
 * that handles a secret is constant time:
 *
 *   - no branch, loop bound or early exit depends on a secret value
 *     (lengths are public: the length of a key, of a record, of a
 *     message);
 *   - no array index and no memory address depends on a secret value
 *     (no table lookup indexed by a key byte or a state byte, such as an
 *     S-box table or a precomputed multiple of a point selected by a
 *     secret scalar digit, unless every entry is read and the wanted one
 *     is selected with masks);
 *   - no division or remainder by or of a secret value (their time
 *     depends on the operands on many CPUs);
 *   - comparisons and selections are done with the cryc_ct_ helpers or
 *     with arithmetic masks, never with ?: or if.
 *
 * A function whose comment says "constant time" follows these rules for
 * the inputs its comment names as secret.  A function that handles only
 * public values (signature verification, certificate parsing) may branch
 * on them.
 *
 * Wiping.  Every structure that holds a secret has a _clear function that
 * overwrites the secret with cryc_wipe; the owner calls it before the
 * memory is freed or reused.  A function that puts a secret in a local
 * array wipes the array before it returns, on every path.  A function that
 * fails after writing part of a secret output wipes that output.
 *
 * Randomness comes only from a cryc_random_fn (below); on a zedBSD system
 * the operating system's getrandom, on Windows BCryptGenRandom
 * (cryc-os.h).  A failure to get random bytes is an error (EIO); there is
 * no fallback.
 *
 * The error values, used alike by every part (errno values; 0 is
 * success):
 *
 *   EINVAL     Malformed input: an encoding that breaks its
 *              specification, a length or a value out of its range, a
 *              point not on its curve; or a caller's argument out of the
 *              range a comment allows.
 *   EBADMSG    An integrity check failed: an AEAD tag, a signature, a
 *              Finished message.
 *   ENOTSUP    The input is valid but uses something outside the range
 *              libssl-compat supports (an algorithm, a curve, a version,
 *              an extension feature).
 *   EPROTO     The peer broke the TLS protocol: a message in the wrong
 *              state, a value it was not offered, a missing extension.
 *   EACCES     A policy refused a valid input: a name outside a name
 *              constraint (cryc-name.h).
 *   ERANGE     A well-formed number is larger than the field it fills
 *              allows (cryc-der.h).
 *   ENOENT     A thing looked for is not there (a file, a further PEM
 *              block).
 *   EMSGSIZE   A record or a message is longer than the protocol allows
 *              (the record_overflow alert).
 *   ENOMEM     An allocation failed.
 *   ENOBUFS    A buffer the caller sized is too small.
 *   EOVERFLOW  A counter would wrap (a record sequence number, a
 *              keystream counter).
 *   EAGAIN     The transport cannot move bytes now (non-blocking I/O).
 *   EIO        The transport or the operating system failed.
 *   ENOSYS     Not implemented (only in the skeleton).
 *
 * No part keeps hidden global state except where its header says so (the
 * error queue of each thread, cryc-err.h); everything else lives in
 * structures the caller owns, so two connections run on two threads
 * without locks.
 *
 * The language is ANSI C (C89) with <stdint.h>; 64-bit integers are
 * uint64_t and int64_t; a 64-bit constant is built from two 32-bit
 * halves, not written as a long long literal.
 */

#ifndef LIBCRYPTO_COMPAT_CRYC_H
#define LIBCRYPTO_COMPAT_CRYC_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Marks a parameter a function does not use, so the compiler does not
 * warn about it.
 */
#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(name) ((void)(name))
#endif

/*
 * Builds a uint64_t from its high and low 32-bit halves (a C89 program
 * cannot write a 64-bit literal without long long).
 */
#define CRYC_U64(high, low) ((((uint64_t)(high)) << 32) | (uint64_t)(low))

/*
 * A source of random bytes: fills size bytes at out and returns 0, or
 * EIO when the source failed (out is then wiped).  context is the
 * source's own state (NULL for the operating system's source).  The
 * library uses cryc_os_random (cryc-os.h); a test passes a deterministic
 * source.
 */
typedef int (*cryc_random_fn)(void *context, uint8_t *out, size_t size);

/*
 * Overwrites size bytes at memory with zeros in a way the compiler does
 * not remove (the stores go through a volatile pointer).
 *
 * memory may be NULL only when size is 0.  Used for every secret before
 * its memory is freed, reused or goes out of scope.
 */
void cryc_wipe(void *memory, size_t size);

/*
 * Compares size bytes of left and right in constant time.
 *
 * Returns 1 when they are equal and 0 otherwise.  The time depends only on
 * size, not on the contents or on where they differ.  left and right may
 * be NULL only when size is 0 (the result is then 1).
 */
int cryc_ct_equal(const uint8_t *left, const uint8_t *right, size_t size);

/* Returns 0xffffffff when value is 0 and 0 otherwise, in constant time. */
uint32_t cryc_ct_mask_zero32(uint32_t value);

/* Returns 0xffffffff when left equals right and 0 otherwise, in constant time. */
uint32_t cryc_ct_mask_eq32(uint32_t left, uint32_t right);

/* Returns 0xffffffff when left is less than right (unsigned) and 0 otherwise, in constant time. */
uint32_t cryc_ct_mask_lt32(uint32_t left, uint32_t right);

/*
 * Copies size bytes into out: from when_set where mask is 0xffffffff, from
 * when_clear where mask is 0; in constant time.
 *
 * mask must be 0 or 0xffffffff (the result for other values is a bitwise
 * mix and is a caller's mistake).  out may be the same as when_set or
 * when_clear.
 */
void cryc_ct_select_bytes(uint8_t *out, const uint8_t *when_set, const uint8_t *when_clear, size_t size, uint32_t mask);

/*
 * Moves size bytes from source to destination when mask is 0xffffffff
 * and leaves destination unchanged when mask is 0, in constant time (a
 * conditional copy, e.g. of a decrypted value only when its tag was
 * good).
 */
void cryc_ct_copy_if(uint8_t *destination, const uint8_t *source, size_t size, uint32_t mask);

/* Reads a 16-bit big-endian integer from bytes[0..1]. */
uint32_t cryc_load_be16(const uint8_t *bytes);

/* Reads a 24-bit big-endian integer from bytes[0..2]. */
uint32_t cryc_load_be24(const uint8_t *bytes);

/* Reads a 32-bit big-endian integer from bytes[0..3]. */
uint32_t cryc_load_be32(const uint8_t *bytes);

/* Reads a 64-bit big-endian integer from bytes[0..7]. */
uint64_t cryc_load_be64(const uint8_t *bytes);

/* Reads a 32-bit little-endian integer from bytes[0..3]. */
uint32_t cryc_load_le32(const uint8_t *bytes);

/* Writes the low 16 bits of value big-endian into bytes[0..1]. */
void cryc_store_be16(uint8_t *bytes, uint32_t value);

/* Writes the low 24 bits of value big-endian into bytes[0..2]. */
void cryc_store_be24(uint8_t *bytes, uint32_t value);

/* Writes value big-endian into bytes[0..3]. */
void cryc_store_be32(uint8_t *bytes, uint32_t value);

/* Writes value big-endian into bytes[0..7]. */
void cryc_store_be64(uint8_t *bytes, uint64_t value);

/* Writes value little-endian into bytes[0..3]. */
void cryc_store_le32(uint8_t *bytes, uint32_t value);

/* Writes value little-endian into bytes[0..7]. */
void cryc_store_le64(uint8_t *bytes, uint64_t value);

#endif
