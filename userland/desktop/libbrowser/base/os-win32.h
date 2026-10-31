/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the Windows files of the operating-system layer share (os-win32.c):
 * turning a Windows error into the layer's errno value and converting text
 * between UTF-8 and the UTF-16 the W functions take.
 *
 * Only the os-*-win32.c files include this header; it is compiled for
 * Windows only.  It names no Windows type, so it does not pull in
 * <windows.h> (a file that includes <jpeglib.h> must not see it).
 */

#ifndef KEILAND_BROWSER_OS_WIN32_H
#define KEILAND_BROWSER_OS_WIN32_H

#include "base/os.h"

#include <stddef.h>
#include <wchar.h>

/*
 * Maps a Windows error code (GetLastError, or an HRESULT's code part the
 * caller extracted) to the layer's errno value.
 *
 * code: any value; 0 (ERROR_SUCCESS) maps to 0.
 * Returns:
 *   0             for 0
 *   ENOENT        for ERROR_FILE_NOT_FOUND (2), ERROR_PATH_NOT_FOUND (3),
 *                 ERROR_INVALID_DRIVE (15), ERROR_BAD_NETPATH (53),
 *                 ERROR_BAD_NET_NAME (67), ERROR_MOD_NOT_FOUND (126)
 *   EACCES        for ERROR_ACCESS_DENIED (5), ERROR_WRITE_PROTECT (19)
 *   EBUSY         for ERROR_SHARING_VIOLATION (32), ERROR_LOCK_VIOLATION (33)
 *   ENOMEM        for ERROR_NOT_ENOUGH_MEMORY (8), ERROR_OUTOFMEMORY (14)
 *   ENOSPC        for ERROR_HANDLE_DISK_FULL (39), ERROR_DISK_FULL (112)
 *   ENOTSUP       for ERROR_NOT_SUPPORTED (50), ERROR_CALL_NOT_IMPLEMENTED (120),
 *                 ERROR_PROC_NOT_FOUND (127)
 *   EEXIST        for ERROR_FILE_EXISTS (80), ERROR_ALREADY_EXISTS (183)
 *   EINVAL        for ERROR_INVALID_PARAMETER (87), ERROR_INVALID_NAME (123),
 *                 ERROR_NEGATIVE_SEEK (131), ERROR_BAD_PATHNAME (161)
 *   ENAMETOOLONG  for ERROR_BUFFER_OVERFLOW (111), ERROR_FILENAME_EXCED_RANGE (206)
 *   ERANGE        for ERROR_INSUFFICIENT_BUFFER (122), ERROR_MORE_DATA (234)
 *   ENOTEMPTY     for ERROR_DIR_NOT_EMPTY (145)
 *   ENOTDIR       for ERROR_DIRECTORY (267)
 *   ETIMEDOUT     for WAIT_TIMEOUT (258), ERROR_TIMEOUT (1460)
 *   EILSEQ        for ERROR_NO_UNICODE_TRANSLATION (1113)
 *   EIO           for every other value.
 * The numbers are the values of <winerror.h>; the function is a pure
 * mapping and does not call the system.
 */
int wb_os_error_from_system(unsigned long code);

/*
 * Maps a Windows Sockets error code (WSAGetLastError) to the layer's
 * errno value.
 *
 * code: any value; 0 maps to 0.
 * Returns:
 *   0             for 0
 *   EINTR         for WSAEINTR (10004)
 *   EACCES        for WSAEACCES (10013)
 *   EINVAL        for WSAEFAULT (10014), WSAEINVAL (10022),
 *                 WSANOTINITIALISED (10093)
 *   EMFILE        for WSAEMFILE (10024)
 *   EAGAIN        for WSAEWOULDBLOCK (10035)
 *   EINPROGRESS   for WSAEINPROGRESS (10036)
 *   EALREADY      for WSAEALREADY (10037)
 *   ENOTSOCK      for WSAENOTSOCK (10038)
 *   EMSGSIZE      for WSAEMSGSIZE (10040)
 *   EAFNOSUPPORT  for WSAEPROTONOSUPPORT (10043), WSAEAFNOSUPPORT (10047)
 *   ENOTSUP       for WSAEOPNOTSUPP (10045)
 *   EADDRINUSE    for WSAEADDRINUSE (10048)
 *   EADDRNOTAVAIL for WSAEADDRNOTAVAIL (10049)
 *   ENETDOWN      for WSAENETDOWN (10050)
 *   ENETUNREACH   for WSAENETUNREACH (10051)
 *   ECONNABORTED  for WSAECONNABORTED (10053)
 *   ECONNRESET    for WSAENETRESET (10052), WSAECONNRESET (10054)
 *   ENOBUFS       for WSAENOBUFS (10055)
 *   EISCONN       for WSAEISCONN (10056)
 *   ENOTCONN      for WSAENOTCONN (10057)
 *   EPIPE         for WSAESHUTDOWN (10058)
 *   ETIMEDOUT     for WSAETIMEDOUT (10060)
 *   ECONNREFUSED  for WSAECONNREFUSED (10061)
 *   EHOSTUNREACH  for WSAEHOSTDOWN (10064), WSAEHOSTUNREACH (10065)
 *   ENOMEM        for WSA_NOT_ENOUGH_MEMORY (8)
 *   EIO           for every other value.
 * The numbers are the values of <winsock2.h>; pure mapping.
 */
int wb_os_error_from_socket(int code);

/*
 * Converts UTF-8 text to a new UTF-16 string.
 *
 * text:   NUL-terminated UTF-8; not NULL.  Overlong forms, surrogate code
 *         points (U+D800..U+DFFF), values above U+10FFFF and truncated
 *         sequences are invalid.
 * wide:   receives a malloc'd, NUL-terminated UTF-16 string the caller
 *         frees with free(); NULL on failure.
 * length: when not NULL, receives the number of UTF-16 units before the
 *         NUL (0 on failure).
 * Returns 0, EINVAL (text or wide NULL), EILSEQ (text is not valid UTF-8),
 * ENAMETOOLONG (more than WB_OS_PATH_MAX bytes before the NUL), ENOMEM.
 * Never reads past the NUL of text.
 */
int wb_os_wide_from_utf8(const char *text, wchar_t **wide, size_t *length);

/*
 * Converts UTF-16 from the system to new UTF-8 text.
 *
 * wide:   UTF-16 units; not NULL.
 * count:  how many units to convert, or (size_t)-1 to stop at the first
 *         NUL unit.  A NUL unit inside count is EILSEQ (text the layer
 *         returns never holds a NUL).
 * text:   receives a malloc'd, NUL-terminated UTF-8 string the caller frees
 *         with free(); NULL on failure.
 * length: when not NULL, receives the number of bytes before the NUL (0 on
 *         failure).
 * Returns 0, EINVAL (wide or text NULL), EILSEQ (an unpaired surrogate or
 * an inner NUL), ENAMETOOLONG (the UTF-8 would exceed WB_OS_PATH_MAX bytes),
 * ENOMEM.  With count (size_t)-1 it reads up to and including the first
 * NUL unit and no further.
 */
int wb_os_utf8_from_wide(const wchar_t *wide, size_t count, char **text, size_t *length);

/*
 * Converts a UTF-8 path to a new UTF-16 path for the W file functions.
 *
 * Every '/' becomes '\\'; nothing else changes (no "\\\\?\\" prefix is
 * added: the build's manifest declares long-path awareness).
 * path, wide: as wb_os_wide_from_utf8.  An empty path is EINVAL.
 * Returns 0, EINVAL, EILSEQ, ENAMETOOLONG, ENOMEM.
 */
int wb_os_wide_path(const char *path, wchar_t **wide);

#endif
