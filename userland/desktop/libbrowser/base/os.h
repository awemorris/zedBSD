/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The operating-system layer of the browser engine: everything the engine
 * asks of the system beyond ISO C and the threads of <pthread.h>.
 *
 * The layer is private to libbrowser.  Its groups each have a header of
 * their own (os-system.h, os-file.h, os-net.h, os-time.h) and two
 * implementations, one for the POSIX systems (zedBSD, Linux, FreeBSD:
 * os-*-posix.c) and one for Windows (os-*-win32.c).  The build compiles
 * exactly one of each pair, chosen by the target, never both; no other
 * file of the engine tests the system with #ifdef or includes a system
 * header the layer covers.
 *
 * Conventions shared by every function of the layer:
 *
 *  - Text is UTF-8 and NUL-terminated.  A path is the system's own form
 *    (on Windows both '\\' and '/' separate its parts and a drive letter or
 *    a "\\\\server\\share" prefix starts an absolute one).  On Windows the
 *    Windows implementation converts to UTF-16 itself and calls only the
 *    W functions; a path that is not valid UTF-8 is EILSEQ.
 *  - Fallible functions return 0 on success or one errno value of
 *    <errno.h>, never -1 and never a Windows error code.  The values and
 *    what they mean:
 *      EINVAL        an argument breaks the function's stated contract
 *      ENOENT        the named file, directory or library does not exist
 *      EEXIST        the thing to create exists
 *      EACCES        the system refused access
 *      EBUSY         another process holds the file or its lock
 *      ENOTDIR       a part of a path that must be a directory is not one
 *      ENAMETOOLONG  a path or name is longer than the layer or the
 *                    system accepts
 *      ENOMEM        memory ran out
 *      ENOSPC        the disk is full
 *      ERANGE        the caller's buffer is too small for the answer
 *      EILSEQ        text that must be UTF-8 (or UTF-16 from the system)
 *                    is not
 *      ENOTSUP       the system lacks the facility
 *      EAGAIN        a non-blocking operation would have to wait
 *      EISDIR        a path names a directory where a file is needed
 *      ENOTEMPTY     a directory to remove still holds entries
 *      EMFILE        the process has no descriptor or handle left
 *      EFBIG         a record or a piece of work is larger than allowed
 *      ETIMEDOUT     the system gave up waiting
 *      EIO           any other failure of the system
 *    A function's comment lists which of these it returns and when; the
 *    socket functions add the network values listed in os-net.h.
 *  - A function that fails leaves its outputs as its comment says
 *    (usually untouched, or NULL and 0) and leaks nothing.
 *  - Every function may be called from any thread unless its comment
 *    says otherwise; none keeps hidden state between calls except where
 *    its comment names it (a once-only start-up of the system's library).
 *  - Sizes that are multiplied or added are checked for overflow before
 *    the arithmetic (an overflow is ENOMEM or ENAMETOOLONG as the comment
 *    says), and no function reads or writes outside the buffers its
 *    arguments describe, whatever the system returns.
 *
 * Windows means Windows 10 version 1903 or later (x86_64) and Windows 11
 * (aarch64); the build defines the Windows version macros for Windows 10
 * (0x0A00) and UNICODE.
 */

#ifndef KEILAND_BROWSER_OS_H
#define KEILAND_BROWSER_OS_H

#include <stddef.h>
#include <stdint.h>

/*
 * The longest path, in bytes of UTF-8 without the NUL, the layer accepts or
 * produces.  32767 UTF-16 units is the Windows limit of an extended path;
 * one unit becomes at most three bytes of UTF-8.
 */
#define WB_OS_PATH_MAX		(32767U * 3U)

#endif
