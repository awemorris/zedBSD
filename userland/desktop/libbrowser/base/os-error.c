/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The English texts of the errno values (base/os-error.h).
 */

/* Named by its directory-relative form: the browser app builds this file without the engine's include path. */
#include "os-error.h"

#include <errno.h>
#include <stddef.h>

/*
 * One errno value and its text.
 *
 * The table is searched from the front, so a value that two names share on
 * one system (EAGAIN and EWOULDBLOCK, ENOTSUP and EOPNOTSUPP) is listed
 * under each name and the first row of its number wins.
 */
struct error_row {
	int code;
	const char *text;
};

/*
 * The texts, the ones the POSIX systems' strerror gives (glibc's wording
 * where the systems differ).  They are constant data that lives as long as
 * the program.  Every value the engine and its layer produce is here
 * (os.h and os-net.h list them, plus EPROTO and EPROTONOSUPPORT from the TLS
 * layer and the values the file layer passes through).
 */
static const struct error_row error_rows[] = {
	{ EPERM, "Operation not permitted" },
	{ ENOENT, "No such file or directory" },
	{ EINTR, "Interrupted system call" },
	{ EIO, "Input/output error" },
	{ E2BIG, "Argument list too long" },
	{ EBADF, "Bad file descriptor" },
	{ EAGAIN, "Resource temporarily unavailable" },
	{ ENOMEM, "Cannot allocate memory" },
	{ EACCES, "Permission denied" },
	{ EFAULT, "Bad address" },
	{ EBUSY, "Device or resource busy" },
	{ EEXIST, "File exists" },
	{ ENODEV, "No such device" },
	{ ENOTDIR, "Not a directory" },
	{ EISDIR, "Is a directory" },
	{ EINVAL, "Invalid argument" },
	{ ENFILE, "Too many open files in system" },
	{ EMFILE, "Too many open files" },
	{ ETXTBSY, "Text file busy" },
	{ EFBIG, "File too large" },
	{ ENOSPC, "No space left on device" },
	{ EROFS, "Read-only file system" },
	{ EPIPE, "Broken pipe" },
	{ ERANGE, "Numerical result out of range" },
	{ ENAMETOOLONG, "File name too long" },
	{ ENOSYS, "Function not implemented" },
	{ ENOTEMPTY, "Directory not empty" },
	{ ELOOP, "Too many levels of symbolic links" },
	{ EWOULDBLOCK, "Resource temporarily unavailable" },
	{ EPROTO, "Protocol error" },
	{ EOVERFLOW, "Value too large for defined data type" },
	{ EILSEQ, "Invalid or incomplete multibyte or wide character" },
	{ ENOTSOCK, "Socket operation on non-socket" },
	{ EMSGSIZE, "Message too long" },
	{ EPROTONOSUPPORT, "Protocol not supported" },
	{ ENOTSUP, "Operation not supported" },
	{ EOPNOTSUPP, "Operation not supported" },
	{ EAFNOSUPPORT, "Address family not supported by protocol" },
	{ EADDRINUSE, "Address already in use" },
	{ EADDRNOTAVAIL, "Cannot assign requested address" },
	{ ENETDOWN, "Network is down" },
	{ ENETUNREACH, "Network is unreachable" },
	{ ECONNABORTED, "Software caused connection abort" },
	{ ECONNRESET, "Connection reset by peer" },
	{ ENOBUFS, "No buffer space available" },
	{ EISCONN, "Transport endpoint is already connected" },
	{ ENOTCONN, "Transport endpoint is not connected" },
	{ ETIMEDOUT, "Connection timed out" },
	{ ECONNREFUSED, "Connection refused" },
	{ EHOSTUNREACH, "No route to host" },
	{ EALREADY, "Operation already in progress" },
	{ EINPROGRESS, "Operation now in progress" },
#ifdef EDQUOT
	{ EDQUOT, "Disk quota exceeded" },
#endif
	{ 0, "Success" }
};

const char *
wb_os_error_text(
	int error)
{
	size_t index;

	/* The text of the first row that has this number. */
	for (index = 0; index < sizeof(error_rows) / sizeof(error_rows[0]); index++) {
		if (error_rows[index].code == error)
			return error_rows[index].text;
	}

	/* Reports a code the table does not know. */
	return "Unknown error";
}
