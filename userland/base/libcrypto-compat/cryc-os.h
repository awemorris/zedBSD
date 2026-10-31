/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The operating system's services the two libraries use: random bytes, the
 * time, a mutex, a value per thread, file reading, socket I/O, and the
 * system's trusted root certificates.
 *
 * Two implementations: cryc-os-posix.c (zedBSD; also Linux and the host
 * of the unit tests) and cryc-os-win32.c (Windows 10 1903 and later, x64
 * and arm64, built with llvm-mingw against the UCRT).  No other file
 * includes an operating-system header beyond the C library's; no other
 * file contains #ifdef _WIN32.
 *
 * Every function is safe to call from several threads at once except
 * where its comment says otherwise.
 */

#ifndef LIBCRYPTO_COMPAT_CRYC_OS_H
#define LIBCRYPTO_COMPAT_CRYC_OS_H

#include "cryc.h"

/* The largest file cryc_os_read_file reads (a CA bundle is about 200 KiB; 16 MiB bounds a hostile one). */
#define CRYC_OS_FILE_MAX	(16UL * 1024UL * 1024UL)

/*
 * The zedBSD (and POSIX) path of the default CA bundle: the
 * ca-certificates package installs it, and OpenSSL's package is built
 * with --openssldir=/etc/ssl so that both find the same file.
 */
#define CRYC_OS_DEFAULT_CA_FILE	"/etc/ssl/cert.pem"

/*
 * The name of the CA bundle a Windows build reads from the directory of
 * the process's executable (the .exe that was started, which the static
 * libraries are linked into).
 */
#define CRYC_OS_WIN32_CA_FILE	"cert.pem"

/*
 * A mutex.  impl is the operating system's object, allocated by
 * cryc_os_mutex_init (a pthread_mutex_t, or an SRWLOCK on Windows) and
 * freed by cryc_os_mutex_destroy.  A structure of all zeros is not
 * usable.
 */
struct cryc_os_mutex {
	void *impl;
};

/*
 * Fills size bytes at out with random bytes from the operating system:
 * getrandom(out, n, 0) in a loop (EINTR retried, at most 256 bytes per
 * call as getentropy allows on every system) on POSIX;
 * BCryptGenRandom(NULL, out, n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) on
 * Windows.
 *
 * Has the type cryc_random_fn; context is unused (NULL).  Returns 0, or
 * EIO when the system refused (out is then wiped).  Never falls back to
 * a weaker source.
 */
int cryc_os_random(void *context, uint8_t *out, size_t size);

/* Returns the current time as seconds since 1970-01-01T00:00:00Z (time(NULL); the UCRT's time is 64-bit). */
int64_t cryc_os_time(void);

/*
 * Makes a mutex.  Returns 0 or ENOMEM.
 */
int cryc_os_mutex_init(struct cryc_os_mutex *mutex);

/* Locks a mutex made by cryc_os_mutex_init (not recursive). */
void cryc_os_mutex_lock(struct cryc_os_mutex *mutex);

/* Unlocks a mutex the calling thread holds. */
void cryc_os_mutex_unlock(struct cryc_os_mutex *mutex);

/* Frees a mutex no thread holds; impl becomes NULL.  A NULL impl is ignored. */
void cryc_os_mutex_destroy(struct cryc_os_mutex *mutex);

/*
 * Atomically adds delta (+1 or -1) to *count and returns the new value
 * (reference counts of the public objects).  count is aligned and only
 * changed through this function.
 */
long cryc_os_atomic_add(volatile long *count, long delta);

/*
 * Returns the calling thread's value (NULL until the thread sets one).
 *
 * There is one value per thread for the whole library (the error queue
 * of cryc-err.h uses it).
 */
void *cryc_os_thread_value(void);

/*
 * Sets the calling thread's value.  destroy is called with the value when
 * the thread exits (pthread_key_create's destructor; FlsAlloc's callback
 * on Windows); it must be the same function on every call.  The first call
 * in the process creates the key (pthread_once; InitOnceExecuteOnce).
 * The key is never deleted (no FlsFree, no DllMain): on Windows the static
 * library is linked into the executable, whose code stays mapped until the
 * process ends, so the callback is there while any thread can still exit.
 *
 * Returns 0, or ENOMEM when the key cannot be created or the value
 * cannot be stored.
 */
int cryc_os_thread_set_value(void *value, void (*destroy)(void *));

/*
 * Reads a whole file into a new buffer.
 *
 * path is UTF-8 (converted to UTF-16 for _wfopen on Windows).  *data
 * receives a malloc'd buffer of *size bytes (plus one zero byte after
 * them, not counted, so text parsers may stop at it); the caller frees it.
 * Returns 0; ENOENT when the file does not exist; EFBIG when it is larger
 * than max bytes (max at most CRYC_OS_FILE_MAX); ENOMEM; EIO for other
 * read failures.  On an error *data is NULL and *size 0.
 */
int cryc_os_read_file(const char *path, size_t max, uint8_t **data, size_t *size);

/*
 * Reads up to size bytes from a connected socket (or, on POSIX, any file
 * descriptor) without waiting longer than the socket's own mode makes it.
 *
 * descriptor is the value given to SSL_set_fd: a file descriptor on POSIX
 * (read()), a SOCKET on Windows (recv()).  *done receives the bytes read.
 * Returns 0 with *done > 0; 0 with *done == 0 at the end of the stream;
 * EAGAIN when a non-blocking socket has nothing now (EWOULDBLOCK and
 * WSAEWOULDBLOCK included); EINTR is retried inside; EIO for any other
 * failure, with *os_error set to the system's code (errno, or
 * WSAGetLastError() on Windows) and, on Windows, errno also set to the
 * nearest errno value so that a program written for OpenSSL's
 * SSL_ERROR_SYSCALL reads something meaningful.
 */
int cryc_os_socket_read(intptr_t descriptor, uint8_t *buffer, size_t size, size_t *done, int *os_error);

/*
 * Writes up to size bytes; a partial write sets *done below size and
 * returns 0.  On POSIX: send(..., MSG_NOSIGNAL) where the system defines
 * MSG_NOSIGNAL, falling back to write() when the descriptor is not a
 * socket (ENOTSOCK); write() where it does not (zedBSD today: a program
 * there that writes to sockets handles SIGPIPE itself).  On Windows:
 * send().  Results as cryc_os_socket_read (EAGAIN when nothing could be
 * written now; EPIPE, ECONNRESET and the like are EIO with *os_error).
 */
int cryc_os_socket_write(intptr_t descriptor, const uint8_t *buffer, size_t size, size_t *done, int *os_error);

/*
 * Hands the system's trust decisions to the three callbacks, in this
 * order:
 *
 *   POSIX:   add_file(context, CRYC_OS_DEFAULT_CA_FILE).  add_distrusted
 *            and add_der are not called (the bundle holds only trusted
 *            roots).
 *   Windows: first every certificate of the current user's and the local
 *            machine's "Disallowed" system store, as DER through
 *            add_distrusted; then every certificate of the current user's
 *            and the local machine's "ROOT" system store, as DER through
 *            add_der, except those whose SHA-256 of the DER is also that
 *            of a Disallowed certificate (CertOpenSystemStoreW,
 *            CertEnumCertificatesInStore; a store that cannot be opened
 *            is taken as empty); then add_file(context, <directory of the
 *            process's executable>\CRYC_OS_WIN32_CA_FILE)
 *            (GetModuleFileNameW(NULL, ...) with a buffer grown up to
 *            32768 UTF-16 units, the part before the last backslash,
 *            converted to UTF-8; a longer path is skipped as if the file
 *            were missing).
 *
 * A callback's error is passed back at once; a missing file or an empty
 * store is not an error (the callbacks report ENOENT for a missing file,
 * which this function ignores).  Returns 0 or the first error other than
 * ENOENT.
 */
int cryc_os_system_roots(int (*add_distrusted)(void *context, const uint8_t *der, size_t size), int (*add_der)(void *context, const uint8_t *der, size_t size), int (*add_file)(void *context, const char *path), void *context);

#endif
