/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The system services of the operating-system layer that are neither
 * files, sockets nor clocks: aligned memory, the system's random bytes,
 * thread names, loading a shared library at run time, the names the engine
 * gives the system to web pages, where the bundled data lives, and the
 * system's trusted certificates.
 *
 * os-system-posix.c and os-system-win32.c implement it; the conventions are
 * those of os.h.
 */

#ifndef KEILAND_BROWSER_OS_SYSTEM_H
#define KEILAND_BROWSER_OS_SYSTEM_H

#include "base/os.h"

#include <stddef.h>

/*
 * A shared library the layer loaded (a POSIX dlopen handle or a Windows
 * HMODULE behind an opaque type).  wb_os_library_open creates it and
 * wb_os_library_close destroys it; it is not shared between opens.
 */
struct wb_os_library;

/*
 * A function looked up in a library.  The caller casts it to the function's
 * real type before calling; the layer never calls it.  It is a function
 * pointer type, not void *, because ISO C does not convert between the two.
 */
typedef void (*wb_os_function)(void);

/*
 * Called once for each certificate wb_os_trust_roots finds.
 *
 * context:     the caller's pointer, passed through.
 * der:         the certificate in DER, valid only during the call.
 * length:      bytes at der (more than 0).
 * distrusted:  0 for a certificate the system trusts as a root, 1 for one
 *              the system says never to trust (Windows' "Disallowed"
 *              store), which the caller must reject even when it also
 *              appears as a root.
 * Returns 0 to go on, or an errno value to stop the enumeration;
 * wb_os_trust_roots then returns that value.
 */
typedef int (*wb_os_trust_each)(void *context, const unsigned char *der, size_t length, int distrusted);

/*
 * Allocates memory whose address is a multiple of alignment.
 *
 * alignment: a power of two, at least sizeof(void *), at most 65536.
 * size:      bytes; more than 0.
 * memory:    receives the block, or NULL on failure.  Its contents are
 *            unspecified.  It is freed only with wb_os_aligned_free (never
 *            free(): Windows' _aligned_malloc block is not a malloc block).
 * Returns 0, EINVAL (memory NULL, size 0, alignment out of range or not a
 * power of two), ENOMEM.
 * POSIX: posix_memalign.  Windows: _aligned_malloc.
 */
int wb_os_aligned_alloc(size_t alignment, size_t size, void **memory);

/*
 * Frees a block from wb_os_aligned_alloc.  NULL does nothing.
 * POSIX: free.  Windows: _aligned_free.
 */
void wb_os_aligned_free(void *memory);

/*
 * Fills bytes with random bytes from the system's cryptographically secure
 * generator (for crypto.getRandomValues and randomUUID).
 *
 * bytes:  length bytes to fill; may be NULL only when length is 0.
 * length: any size, including 0 (which does nothing and succeeds).
 * Returns 0, EINVAL (bytes NULL with length above 0), EIO (the generator
 * failed; bytes may then hold anything).  Never fills with a predictable
 * fallback.
 * POSIX: reads /dev/urandom (as the engine does today), retrying short
 * reads and EINTR.  Windows: BCryptGenRandom with
 * BCRYPT_USE_SYSTEM_PREFERRED_RNG, in pieces of at most ULONG_MAX bytes.
 */
int wb_os_random(void *bytes, size_t length);

/*
 * Names the calling thread for debuggers and the system's tools.
 *
 * name: NUL-terminated ASCII, at most 15 bytes are used (longer names are
 *       cut); NULL does nothing.
 * Best effort: a system without the facility ignores it.
 * POSIX: pthread_setname_np on Linux and FreeBSD, nothing on zedBSD (the
 * behaviour of the engine today).  Windows: SetThreadDescription, looked
 * up at run time in kernel32.dll (absent before Windows 10 1607).
 */
void wb_os_thread_name(const char *name);

/*
 * Loads the first library of a list that the system can load.
 *
 * names:   NULL-terminated array of file names (no directory), tried in
 *          order; not NULL and not empty.
 * library: receives the library, or NULL on failure.
 * Returns 0, EINVAL (names or library NULL, names empty or a name empty),
 * ENOENT (no name could be loaded), ENOMEM.
 * POSIX: dlopen(name, RTLD_NOW | RTLD_LOCAL).  Windows: LoadLibraryExW with
 * LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32 only
 * (never the current directory or PATH), so a planted DLL is not loaded.
 */
int wb_os_library_open(const char *const *names, struct wb_os_library **library);

/*
 * Looks up a function in a loaded library.
 *
 * library: from wb_os_library_open; not NULL.
 * name:    the exported name; not NULL.
 * Returns the function, or NULL when the library does not export it (or an
 * argument is NULL).
 */
wb_os_function wb_os_library_symbol(struct wb_os_library *library, const char *name);

/*
 * Unloads a library and frees it.  NULL does nothing.  Functions looked up
 * in it must not be called afterwards.
 */
void wb_os_library_close(struct wb_os_library *library);

/*
 * The value of navigator.platform: POSIX gives the operating system's name
 * and the processor ("zedBSD x86_64" on zedBSD, from include/kern/os-name.h
 * and the compiler's target, exactly the engine's value today); Windows
 * gives "Win32" on every processor (what browsers on Windows report).
 * Returns a static string; never NULL.
 */
const char *wb_os_platform_name(void);

/*
 * The system part of the User-Agent (the text between the first
 * parentheses): POSIX gives "X11; Linux x86_64" (the engine's value today
 * on every POSIX system); Windows gives "Windows NT 10.0; Win64; x64" on
 * every processor (what browsers on Windows send, arm64 included).
 * Returns a static string; never NULL.
 */
const char *wb_os_user_agent_system(void);

/*
 * Writes the directory of the bundled read-only data (fonts, pages) into
 * buffer, without a trailing separator.
 *
 * buffer: size bytes; receives NUL-terminated UTF-8.
 * size:   bytes at buffer; must be more than 0.
 * Returns 0, EINVAL (buffer NULL or size 0), ERANGE (the path and its NUL
 * do not fit; buffer then holds an empty string), EILSEQ, EIO.
 * POSIX: KEILAND_DATADIR of userland/desktop/paths.h ("/usr/share").
 * Windows: the directory of the running .exe (GetModuleFileNameW of NULL)
 * followed by "\\share".
 */
int wb_os_data_directory(char *buffer, size_t size);

/*
 * Hands each certificate of the system's trust stores to each.
 *
 * each:    called once per certificate; not NULL.
 * context: passed to each.
 * Returns 0 when every certificate was handed over (also when the system
 * has none or keeps them where the TLS library reads them itself), the
 * value each returned to stop, EINVAL (each NULL), EIO (a store could not
 * be opened or read).
 * POSIX: calls each for nothing and returns 0 (OpenSSL reads the system's
 * file itself).  Windows: every certificate of the current user's and the
 * machine's "ROOT" stores with distrusted 0, then every certificate of
 * their "Disallowed" stores with distrusted 1 (CertOpenStore with
 * CERT_STORE_PROV_SYSTEM_W, read-only, and CertEnumCertificatesInStore);
 * a certificate whose encoding is not X.509 ASN.1 is skipped.
 */
int wb_os_trust_roots(wb_os_trust_each each, void *context);

#endif
