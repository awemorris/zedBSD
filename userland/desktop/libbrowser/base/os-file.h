/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The files of the operating-system layer: whole-file reading and writing,
 * replacing a file so a reader sees the old or the new bytes and never a
 * mixture, removing, directories, the absolute and current paths, the
 * mapping between a local path and a file: URL, and a lock one process
 * holds on a directory.
 *
 * os-file-posix.c and os-file-win32.c implement it; the conventions are
 * those of os.h.  Results that grow (file contents, paths) are appended to
 * a struct wb_buffer of base.h, which keeps a NUL after its bytes; on
 * failure a function leaves the buffer's length as it found it (the
 * capacity may have grown).
 */

#ifndef KEILAND_BROWSER_OS_FILE_H
#define KEILAND_BROWSER_OS_FILE_H

#include "base/base.h"
#include "base/os.h"

#include <stddef.h>

/*
 * A lock held on a directory by this process (a POSIX descriptor with
 * flock, or a Windows handle with LockFileEx, behind an opaque type).
 * wb_os_lock_open creates it and wb_os_lock_close releases and frees it;
 * the system also releases it when the process ends, however it ends.
 */
struct wb_os_lock;

/*
 * Called once for each name wb_os_list finds.
 *
 * context: the caller's pointer.
 * name:    the entry's name, UTF-8, NUL-terminated, never "." or "..";
 *          valid only during the call.
 * Returns 0 to go on, or an errno value to stop; wb_os_list then returns it.
 */
typedef int (*wb_os_list_each)(void *context, const char *name);

/*
 * Appends the whole contents of a file to buffer.
 *
 * path:   the file; not NULL, not empty.
 * buffer: an initialized buffer; the bytes are appended after its current
 *         length and a NUL is kept after them (the NUL is not counted).
 * Returns 0, EINVAL (an argument NULL or path empty), ENOENT, EACCES,
 * EISDIR (path names a directory), EBUSY (Windows: another process holds
 * it without sharing reads), EILSEQ, ENAMETOOLONG, ENOMEM, EIO.
 * The file is read in binary (Windows: no CR-LF translation); a read cut
 * by a signal is retried.
 */
int wb_os_file_read(const char *path, struct wb_buffer *buffer);

/*
 * Creates or empties a file and writes length bytes to it.
 *
 * path:   not NULL, not empty.  Its directory must exist.
 * bytes:  length bytes; may be NULL only when length is 0.
 * Returns 0, EINVAL, ENOENT (the directory is missing), EACCES, EISDIR,
 * EBUSY, ENOSPC, EILSEQ, ENAMETOOLONG, ENOMEM, EIO.  A failure part way
 * leaves the file with unspecified contents (use wb_os_file_replace when
 * that matters).  Binary; POSIX creates it with mode 0644.
 */
int wb_os_file_write(const char *path, const void *bytes, size_t length);

/*
 * Replaces a file's contents as one step: writes the bytes to a new file
 * in the same directory, flushes it to the disk, then renames it over path.
 * A reader, or the file after a crash, holds either the old or the new
 * contents in full.
 *
 * path, bytes, length: as wb_os_file_write.
 * The temporary file is named path followed by ".tmp" and a decimal
 * number unique within the process (a counter) and the process ID, and is
 * removed again when any step fails.
 * Returns the errors of wb_os_file_write.  Windows: the rename is
 * MoveFileExW with MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH; when
 * it fails with EACCES or EBUSY (another program, such as a virus scanner,
 * holds path for a moment) it is retried up to 5 more times, 20 ms apart,
 * before the error is returned.  POSIX: fsync then rename.
 */
int wb_os_file_replace(const char *path, const void *bytes, size_t length);

/*
 * Removes a file (not a directory).
 *
 * Returns 0, EINVAL, ENOENT (nothing to remove), EACCES, EISDIR, EBUSY,
 * EILSEQ, ENAMETOOLONG, EIO.
 */
int wb_os_file_remove(const char *path);

/*
 * Creates a directory and every missing directory above it.
 *
 * path: not NULL, not empty.  A trailing separator is allowed.
 * Returns 0 (also when the directory exists already), EINVAL, ENOTDIR (a
 * part of path is a file), EACCES, ENOSPC, EILSEQ, ENAMETOOLONG, ENOMEM,
 * EIO.  POSIX creates with mode 0755.
 */
int wb_os_make_directories(const char *path);

/*
 * Calls each with every name in a directory, in no particular order.
 *
 * path:    the directory; not NULL, not empty.
 * each:    not NULL.
 * context: passed to each.
 * Returns 0 after the last name, the value each returned to stop, EINVAL,
 * ENOENT, ENOTDIR, EACCES, ENOMEM, EIO.  A name the system gives that is
 * not valid UTF-8 (POSIX) or UTF-16 (Windows) is skipped, not an error.
 * Entries added or removed during the walk may or may not be seen.
 */
int wb_os_list(const char *path, wb_os_list_each each, void *context);

/*
 * Appends the absolute form of path to buffer.
 *
 * POSIX: realpath (symbolic links resolved; the file must exist, ENOENT
 * otherwise).  Windows: GetFullPathNameW (resolved against the current
 * directory and drive, "." and ".." removed; existence is not checked).
 * Returns 0, EINVAL, ENOENT, EACCES, EILSEQ, ENAMETOOLONG, ENOMEM, EIO.
 */
int wb_os_full_path(const char *path, struct wb_buffer *buffer);

/*
 * Appends the current working directory to buffer, without a trailing
 * separator unless it is a root ("/" or "C:\\").
 *
 * Returns 0, EINVAL, ENOENT (the directory was removed), EACCES, EILSEQ,
 * ENAMETOOLONG, ENOMEM, EIO.
 */
int wb_os_current_directory(struct wb_buffer *buffer);

/*
 * Appends the local path a file: URL names.
 *
 * host: the URL's host, already decoded; NULL or "" for this machine.
 * path: the URL's path, percent-decoded (bytes, NUL-terminated); not NULL.
 * POSIX: an empty host gives path unchanged; any other host is ENOENT
 * (as the engine behaves today).
 * Windows:
 *  - empty host and a path "/X:" or "/X:/rest" or "/X|/rest" (X an ASCII
 *    letter): "X:\\rest" with every '/' turned into '\\' ("/X:" alone
 *    gives "X:\\");
 *  - empty host and any other path: ENOENT (no drive to resolve against);
 *  - a host: the UNC path "\\\\host" followed by path with every '/'
 *    turned into '\\'; a path that is empty or "/" alone (no share) is
 *    ENOENT.
 * Returns 0, EINVAL (path or buffer NULL), ENOENT, ENOMEM.  The bytes are
 * not checked as UTF-8 here (the file functions do that when the path is
 * used).
 */
int wb_os_path_from_url(const char *host, const char *path, struct wb_buffer *buffer);

/*
 * Splits an absolute local path into the host and the path of its file:
 * URL, neither percent-encoded (the caller encodes).
 *
 * local: an absolute path; not NULL.
 * host:  receives the host: nothing appended for this machine, the server
 *        for a Windows UNC path.
 * path:  receives the URL path, starting with '/'.
 * POSIX: local must start with '/' (EINVAL otherwise); host gets nothing
 * and path gets local unchanged.
 * Windows: "X:\\rest" (or "X:/rest") gives path "/X:/rest" with every '\\'
 * turned into '/'; "\\\\server\\share\\rest" gives host "server" and path
 * "/share/rest"; a relative path, "X:rest" (relative to a drive) or a
 * "\\\\?\\" or "\\\\.\\" device path is EINVAL.
 * Returns 0, EINVAL, ENOMEM.  On failure neither buffer's length changes.
 */
int wb_os_url_from_path(const char *local, struct wb_buffer *host, struct wb_buffer *path);

/*
 * Takes this process's lock on a directory, creating the lock file
 * "<directory>/lock" when it is missing.  The lock is advisory: it keeps a
 * second process using the same storage directory out, nothing else.
 *
 * directory: an existing directory; not NULL.
 * lock:      receives the lock, or NULL on failure.
 * Returns 0, EINVAL, ENOENT (no directory), EBUSY (another process holds
 * it; the call does not wait), EACCES, EILSEQ, ENAMETOOLONG, ENOMEM, EIO.
 * POSIX: open with O_CLOEXEC and flock(LOCK_EX | LOCK_NB).  Windows:
 * CreateFileW with FILE_SHARE_READ | FILE_SHARE_WRITE, not inherited, and
 * LockFileEx(LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY) on its
 * first byte.  Taking the same directory twice in one process is EBUSY on
 * both systems (POSIX keeps a list of the directories it holds for this,
 * since flock on a second descriptor of the same process would succeed on
 * some systems).
 */
int wb_os_lock_open(const char *directory, struct wb_os_lock **lock);

/*
 * Releases a lock and frees it.  The lock file stays.  NULL does nothing.
 */
void wb_os_lock_close(struct wb_os_lock *lock);

#endif
