/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The storage backend of the Web browser engine: what keeps cookies, the
 * HTTP cache and localStorage across runs of a program.
 *
 * A program that wants the engine to remember anything gives an instance
 * of the engine (struct browser_instance of <browser/browser.h>) a table of
 * calls and a context; the engine reads and writes records through them.
 * The engine ships one backend, files (browser_storage_files_open), which
 * keeps each record in a file of a directory the program chooses; a
 * program may bring its own (a test's fake, a database).  An instance
 * without a backend remembers nothing beyond its lifetime.
 *
 * What a backend sees:
 *  - A space (BROWSER_STORAGE_*) and a key name each record.  A key is 1
 *    to BROWSER_STORAGE_KEY_MAX characters of [0-9a-f-]: safe as a file
 *    name on every system.  The engine makes the keys; the backend only
 *    stores them.
 *  - A record is bytes the engine encodes and checks itself (each holds
 *    its own version and its original key); a backend never interprets
 *    them and may hand back anything: the engine treats a record that does
 *    not decode as absent.
 *  - The engine calls a backend from one thread only (the instance's
 *    storage thread), one call at a time, so a backend needs no lock of its
 *    own.  A call may block (disk); nobody waits for it but that thread
 *    and, for a read, the one thread that asked.
 *  - A call must not call into the engine (no instance or view function),
 *    except browser_storage_sink_append during a read.
 *  - Memory does not cross between the engine and the backend: a read
 *    appends to the engine's sink, keys and bytes passed in are borrowed
 *    for the call.  So a backend in another library (another C runtime on
 *    Windows) works.
 *  - Errors are errno values: 0, ENOSPC (no room), EIO (could not read or
 *    write), ENOMEM, or any other value, which the engine treats as EIO.
 *    A failing write leaves the engine's memory as it is (the record is
 *    written again at its next change); a failing read is a missing record.
 */

#ifndef BROWSER_STORAGE_H
#define BROWSER_STORAGE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The version of struct browser_storage_calls; a table of another version is refused (ENOTSUP). */
#define BROWSER_STORAGE_VERSION		1

/* The longest key, in characters (without the NUL). */
#define BROWSER_STORAGE_KEY_MAX		200

/*
 * The spaces.  New spaces may be added in later versions with new numbers;
 * a backend stores a space it does not know like any other.
 */
#define BROWSER_STORAGE_COOKIES		1U	/* cookies that have an expiry (persistent cookies) */
#define BROWSER_STORAGE_CACHE		2U	/* responses of the HTTP cache */
#define BROWSER_STORAGE_LOCAL		3U	/* one origin's localStorage area per record */

/*
 * Where a backend's read puts a record's bytes: the engine's buffer,
 * opaque to the backend.
 */
struct browser_storage_sink;

/*
 * The calls of a backend.  Every member but close must be set.  Each takes
 * the context given with the table.
 *
 * read:   finds the record of space and key.  When it exists, appends all
 *         of its bytes to sink (in one or more browser_storage_sink_append
 *         calls) and sets *found to 1; when it does not, appends nothing,
 *         sets *found to 0 and returns 0.  Returns 0 or an error (the sink
 *         is then discarded).
 * write:  creates or replaces the record of space and key with length
 *         bytes (length may be 0).  A reader afterwards finds exactly these
 *         bytes, never a mixture with the old ones.
 * remove: removes the record; a missing record is not an error (0).
 * list:   calls each once with every key of space (in any order), stopping
 *         when each returns other than 0 and returning that value.  The key
 *         passed to each is valid only during that call.  The engine does
 *         not write or remove during a list.
 * clear:  removes every record of space.
 * close:  the instance is gone; frees the context.  May be NULL.  Called
 *         exactly once, after the last other call, also when the instance
 *         could not be made.
 */
struct browser_storage_calls {
	int version;
	int (*read)(void *context, unsigned space, const char *key, struct browser_storage_sink *sink, int *found);
	int (*write)(void *context, unsigned space, const char *key, const void *bytes, size_t length);
	int (*remove)(void *context, unsigned space, const char *key);
	int (*list)(void *context, unsigned space, int (*each)(void *each_context, const char *key), void *each_context);
	int (*clear)(void *context, unsigned space);
	void (*close)(void *context);
};

/*
 * Appends bytes of the record being read to sink.  Only a backend's read
 * calls it, with the sink it was given.
 *
 * bytes:  length bytes; NULL only when length is 0.
 * Returns 0, EINVAL (sink NULL, or bytes NULL with length above 0), EFBIG
 * (the record would exceed the engine's largest record, 64 MiB), ENOMEM.
 * After a failure every later append to the same sink fails the same way;
 * the backend should return the error from its read.
 */
int browser_storage_sink_append(struct browser_storage_sink *sink, const void *bytes, size_t length);

/*
 * Opens the files backend over a directory.
 *
 * directory: where the records go (UTF-8; created with its parents when
 *            missing).  Records are files named by their keys in a
 *            directory per space ("cookies", "cache", "local-storage"; a
 *            localStorage record's file name ends in ".ls"), so the
 *            localStorage files of earlier versions of the engine, kept in
 *            "<directory>/local-storage/<origin in hexadecimal>.ls", are
 *            found again.  While open the backend holds a lock on the
 *            directory, so a second program using the same directory is
 *            told so instead of overwriting the first one's records.
 * calls:     receives the backend's table (static; never freed).
 * context:   receives the backend's context, which the table's close frees.
 * Returns 0, EINVAL (an argument NULL or directory empty), EBUSY (another
 * process, or another open in this process, holds the directory), EACCES,
 * ENOTDIR, ENOSPC, ENOMEM, EIO.  On failure nothing is left open and
 * *calls and *context are NULL.
 */
int browser_storage_files_open(const char *directory, const struct browser_storage_calls **calls, void **context);

#ifdef __cplusplus
}
#endif

#endif
