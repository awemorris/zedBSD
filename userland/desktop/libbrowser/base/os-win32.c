/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Windows helpers of the operating-system layer: errors and UTF-16.
 */

#include "base/os-win32.h"
#include "base/base.h"

#include <winsock2.h>
#include <windows.h>

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* The longest path the W file functions take, in UTF-16 units (the limit of an extended-length path). */
#define OS_WIDE_PATH_MAX	32767U

/* The units of the extended-length prefix "\\?\" put before a drive path. */
#define OS_VERBATIM_LENGTH	4U

/* The units of "\\?\UNC\", which takes the place of the leading "\\" of a UNC path. */
#define OS_UNC_VERBATIM_LENGTH	8U

/* The units of the leading "\\" of a UNC path. */
#define OS_UNC_LEAD_LENGTH	2U

/* How many times a full path is asked for again when the answer did not fit the buffer. */
#define OS_FULL_PATH_ATTEMPTS	4

/* The smallest code point written as a surrogate pair. */
#define OS_SUPPLEMENTARY_MIN	0x10000U

static int os_utf8_decode(const unsigned char *text, uint32_t *code, size_t *used);
static int os_wide_encode(const wchar_t *wide, size_t count, char *out, size_t *bytes);
static int os_full_path(const wchar_t *source, wchar_t **full, size_t *full_length);
static int os_full_path_failure(void);
static int os_path_is_extended(const wchar_t *path);
static int os_path_is_drive(const wchar_t *path);

/*
 * Maps a Windows error code to the layer's errno value.
 *
 * It is a pure table lookup and does not call the system.
 */
int
wb_os_error_from_system(
	unsigned long code)
{
	/*
	 * Values referenced: <winerror.h> of mingw-w64 (the system error numbers
	 * that the table in base/os-win32.h names).
	 */
	switch (code) {
	case 0:
		/* ERROR_SUCCESS is no error. */
		return 0;
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:
	case ERROR_INVALID_DRIVE:
	case ERROR_BAD_NETPATH:
	case ERROR_BAD_NET_NAME:
	case ERROR_MOD_NOT_FOUND:
		/* The file, path, drive, network path or module is not there. */
		return ENOENT;
	case ERROR_ACCESS_DENIED:
	case ERROR_WRITE_PROTECT:
		/* The system refused access. */
		return EACCES;
	case ERROR_SHARING_VIOLATION:
	case ERROR_LOCK_VIOLATION:
		/* Another process holds the file or its lock. */
		return EBUSY;
	case ERROR_NOT_ENOUGH_MEMORY:
	case ERROR_OUTOFMEMORY:
		/* Memory ran out. */
		return ENOMEM;
	case ERROR_HANDLE_DISK_FULL:
	case ERROR_DISK_FULL:
		/* The disk is full. */
		return ENOSPC;
	case ERROR_NOT_SUPPORTED:
	case ERROR_CALL_NOT_IMPLEMENTED:
	case ERROR_PROC_NOT_FOUND:
		/* The system lacks the facility. */
		return ENOTSUP;
	case ERROR_FILE_EXISTS:
	case ERROR_ALREADY_EXISTS:
		/* The thing to create exists. */
		return EEXIST;
	case ERROR_INVALID_PARAMETER:
	case ERROR_INVALID_NAME:
	case ERROR_NEGATIVE_SEEK:
	case ERROR_BAD_PATHNAME:
		/* An argument or a name breaks the call's contract. */
		return EINVAL;
	case ERROR_BUFFER_OVERFLOW:
	case ERROR_FILENAME_EXCED_RANGE:
		/* A path or name is longer than the system accepts. */
		return ENAMETOOLONG;
	case ERROR_INSUFFICIENT_BUFFER:
	case ERROR_MORE_DATA:
		/* The caller's buffer is too small for the answer. */
		return ERANGE;
	case ERROR_DIR_NOT_EMPTY:
		/* A directory to remove still holds entries. */
		return ENOTEMPTY;
	case ERROR_DIRECTORY:
		/* A part of a path that must be a directory is not one. */
		return ENOTDIR;
	case WAIT_TIMEOUT:
	case ERROR_TIMEOUT:
		/* The system gave up waiting. */
		return ETIMEDOUT;
	case ERROR_NO_UNICODE_TRANSLATION:
		/* The text is not valid in its encoding. */
		return EILSEQ;
	default:
		break;
	}

	/* Any other failure of the system. */
	return EIO;
}

/*
 * Maps a Windows Sockets error code to the layer's errno value.
 *
 * It is a pure table lookup and does not call the system.
 */
int
wb_os_error_from_socket(
	int code)
{
	/*
	 * Values referenced: <winsock2.h> of mingw-w64 (the Windows Sockets error
	 * numbers that the table in base/os-win32.h names).
	 */
	switch (code) {
	case 0:
		/* No error. */
		return 0;
	case WSAEINTR:
		/* A blocking call was interrupted. */
		return EINTR;
	case WSAEACCES:
		/* The system refused access. */
		return EACCES;
	case WSAEFAULT:
	case WSAEINVAL:
	case WSANOTINITIALISED:
		/* An argument breaks the call's contract, or the sockets library was never started. */
		return EINVAL;
	case WSAEMFILE:
		/* The process has no handle left. */
		return EMFILE;
	case WSAEWOULDBLOCK:
		/* A non-blocking operation would have to wait. */
		return EAGAIN;
	case WSAEINPROGRESS:
		/* A non-blocking operation has started and is not finished. */
		return EINPROGRESS;
	case WSAEALREADY:
		/* The operation is already under way. */
		return EALREADY;
	case WSAENOTSOCK:
		/* The handle is not a socket. */
		return ENOTSOCK;
	case WSAEMSGSIZE:
		/* The message is larger than the socket takes. */
		return EMSGSIZE;
	case WSAEPROTONOSUPPORT:
	case WSAEAFNOSUPPORT:
		/* The protocol or the address family is not supported. */
		return EAFNOSUPPORT;
	case WSAEOPNOTSUPP:
		/* The operation is not supported on this kind of socket. */
		return ENOTSUP;
	case WSAEADDRINUSE:
		/* The address is in use. */
		return EADDRINUSE;
	case WSAEADDRNOTAVAIL:
		/* The address is not one of this machine's. */
		return EADDRNOTAVAIL;
	case WSAENETDOWN:
		/* The network is down. */
		return ENETDOWN;
	case WSAENETUNREACH:
		/* The network cannot be reached. */
		return ENETUNREACH;
	case WSAECONNABORTED:
		/* The local side aborted the connection. */
		return ECONNABORTED;
	case WSAENETRESET:
	case WSAECONNRESET:
		/* The peer or the network reset the connection. */
		return ECONNRESET;
	case WSAENOBUFS:
		/* The system has no buffer space left. */
		return ENOBUFS;
	case WSAEISCONN:
		/* The socket is already connected. */
		return EISCONN;
	case WSAENOTCONN:
		/* The socket is not connected. */
		return ENOTCONN;
	case WSAESHUTDOWN:
		/* The socket was shut down for sending. */
		return EPIPE;
	case WSAETIMEDOUT:
		/* The system gave up waiting. */
		return ETIMEDOUT;
	case WSAECONNREFUSED:
		/* The peer refused the connection. */
		return ECONNREFUSED;
	case WSAEHOSTDOWN:
	case WSAEHOSTUNREACH:
		/* The host is down or cannot be reached. */
		return EHOSTUNREACH;
	case WSA_NOT_ENOUGH_MEMORY:
		/* Memory ran out. */
		return ENOMEM;
	default:
		break;
	}

	/* Any other failure of the system. */
	return EIO;
}

/*
 * Converts UTF-8 text to a new UTF-16 string.
 *
 * The text is validated against the well-formed sequences of the Unicode
 * Standard before anything is allocated.
 */
int
wb_os_wide_from_utf8(
	const char *text,
	wchar_t **wide,
	size_t *length)
{
	const unsigned char *bytes;
	size_t byte_count;
	size_t at;
	size_t used;
	size_t units;
	size_t index;
	uint32_t code;
	wchar_t *result;
	int error;

	/* Leaves the outputs in their failure state until the conversion has worked. */
	if (wide != NULL)
		*wide = NULL;
	if (length != NULL)
		*length = 0;

	/* Refuses a call that names no text or no place for the answer. */
	if (text == NULL || wide == NULL)
		return EINVAL;

	/*
	 * Measures the text, never looking further than the first byte past the
	 * longest length the layer accepts, so a text without a NUL is not
	 * read to its end.
	 */
	bytes = (const unsigned char *)text;
	byte_count = 0;
	while (byte_count <= WB_OS_PATH_MAX && bytes[byte_count] != 0)
		byte_count++;

	/* Refuses a text longer than the layer accepts. */
	if (byte_count > WB_OS_PATH_MAX)
		return ENAMETOOLONG;

	/* Validates every sequence and counts the UTF-16 units they need. */
	at = 0;
	units = 0;
	while (at < byte_count) {
		error = os_utf8_decode(bytes + at, &code, &used);
		if (error != 0)
			return error;

		at += used;
		units++;
		if (code >= OS_SUPPLEMENTARY_MIN)
			units++;
	}

	/* Refuses a count whose byte size would overflow. */
	if (units > (SIZE_MAX / sizeof(wchar_t)) - 1U)
		return ENOMEM;

	/* Allocates the result with room for the NUL. */
	result = (wchar_t *)malloc((units + 1U) * sizeof(wchar_t));
	if (result == NULL)
		return ENOMEM;

	/* Writes the units of every sequence; the text was validated above. */
	at = 0;
	index = 0;
	while (at < byte_count) {
		error = os_utf8_decode(bytes + at, &code, &used);
		if (error != 0) {
			free(result);
			return error;
		}

		at += used;
		if (code >= OS_SUPPLEMENTARY_MIN) {
			/* A supplementary code point becomes a high and a low surrogate. */
			code -= OS_SUPPLEMENTARY_MIN;
			result[index++] = (wchar_t)(0xd800U + (code >> 10));
			result[index++] = (wchar_t)(0xdc00U + (code & 0x3ffU));
		} else {
			result[index++] = (wchar_t)code;
		}
	}

	/* Terminates the string. */
	result[index] = 0;

	/* Hands the string and its length to the caller. */
	*wide = result;
	if (length != NULL)
		*length = index;

	/* Succeeded: the caller owns the string. */
	return 0;
}

/*
 * Converts UTF-16 from the system to new UTF-8 text.
 *
 * The units are measured and checked first, so nothing is allocated for
 * text that cannot be converted.
 */
int
wb_os_utf8_from_wide(
	const wchar_t *wide,
	size_t count,
	char **text,
	size_t *length)
{
	size_t bytes;
	char *result;
	int error;

	/* Leaves the outputs in their failure state until the conversion has worked. */
	if (text != NULL)
		*text = NULL;
	if (length != NULL)
		*length = 0;

	/* Refuses a call that names no units or no place for the answer. */
	if (wide == NULL || text == NULL)
		return EINVAL;

	/* Validates the units and measures the UTF-8 they make. */
	bytes = 0;
	error = os_wide_encode(wide, count, NULL, &bytes);
	if (error != 0)
		return error;

	/* Allocates the result with room for the NUL. */
	result = (char *)malloc(bytes + 1U);
	if (result == NULL)
		return ENOMEM;

	/* Writes the UTF-8; the units were validated above. */
	error = os_wide_encode(wide, count, result, &bytes);
	if (error != 0) {
		free(result);
		return error;
	}

	/* Terminates the text. */
	result[bytes] = '\0';

	/* Hands the text and its length to the caller. */
	*text = result;
	if (length != NULL)
		*length = bytes;

	/* Succeeded: the caller owns the text. */
	return 0;
}

/*
 * Converts a UTF-8 path to a new UTF-16 path for the W file functions.
 *
 * An absolute path is made full and given the extended-length prefix; any
 * other path only has its separators turned.
 */
int
wb_os_wide_path(
	const char *path,
	wchar_t **wide)
{
	wchar_t *converted;
	wchar_t *full;
	wchar_t *result;
	size_t length;
	size_t full_length;
	size_t prefix_length;
	size_t skip_length;
	size_t i;
	int absolute;
	int error;

	/* Leaves the output in its failure state until the conversion has worked. */
	if (wide != NULL)
		*wide = NULL;

	/* Refuses a call that names no path or no place for the answer. */
	if (path == NULL || wide == NULL)
		return EINVAL;

	/* Refuses an empty path, which names nothing. */
	if (path[0] == '\0')
		return EINVAL;

	/* Converts the text to UTF-16. */
	error = wb_os_wide_from_utf8(path, &converted, &length);
	if (error != 0)
		return error;

	/* Turns every '/' into the Windows separator. */
	for (i = 0; i < length; i++) {
		if (converted[i] == L'/')
			converted[i] = L'\\';
	}

	/* Leaves a path that is already extended-length or a device path as it is. */
	if (os_path_is_extended(converted) != 0) {
		*wide = converted;
		return 0;
	}

	/*
	 * Decides whether the path is absolute: a drive letter, a colon and a
	 * separator, or two separators that start a UNC path.  A path with a
	 * drive but no separator after the colon, or one that starts with a
	 * single separator, depends on the current directory and stays as it is.
	 */
	absolute = 0;
	if (os_path_is_drive(converted) != 0) {
		absolute = 1;
	} else if (converted[0] == L'\\' && converted[1] == L'\\') {
		absolute = 1;
	}

	/* Leaves a path that is not absolute with only its separators turned. */
	if (absolute == 0) {
		*wide = converted;
		return 0;
	}

	/* Makes the path full: "." and ".." are removed and the separators are single. */
	error = os_full_path(converted, &full, &full_length);
	free(converted);
	if (error != 0)
		return error;

	/* Leaves a result that came out as an extended-length or device path as it is. */
	if (os_path_is_extended(full) != 0) {
		*wide = full;
		return 0;
	}

	/*
	 * Chooses the prefix: a UNC path loses its two leading separators to
	 * "\\?\UNC\", a drive path gets "\\?\" in front, and any other form is
	 * left without a prefix.
	 */
	prefix_length = 0;
	skip_length = 0;
	if (full[0] == L'\\' && full[1] == L'\\') {
		prefix_length = OS_UNC_VERBATIM_LENGTH;
		skip_length = OS_UNC_LEAD_LENGTH;
	} else if (os_path_is_drive(full) != 0) {
		prefix_length = OS_VERBATIM_LENGTH;
	}

	/* Refuses a path whose prefixed form exceeds the extended-length limit. */
	if (full_length - skip_length + prefix_length > OS_WIDE_PATH_MAX) {
		free(full);
		return ENAMETOOLONG;
	}

	/* Keeps a form without a prefix as the system made it. */
	if (prefix_length == 0) {
		*wide = full;
		return 0;
	}

	/* Allocates the prefixed path with room for the NUL. */
	result = (wchar_t *)malloc((prefix_length + full_length - skip_length + 1U) * sizeof(wchar_t));
	if (result == NULL) {
		free(full);
		return ENOMEM;
	}

	/* Writes the prefix and then the rest of the full path, with its NUL. */
	if (skip_length != 0)
		wmemcpy(result, L"\\\\?\\UNC\\", prefix_length);
	else
		wmemcpy(result, L"\\\\?\\", prefix_length);
	wmemcpy(result + prefix_length, full + skip_length, full_length - skip_length + 1U);
	free(full);

	/* Hands the path to the caller. */
	*wide = result;

	/* Succeeded: the caller owns the path. */
	return 0;
}

/*
 * Decodes one well-formed UTF-8 sequence.
 *
 * Values referenced: the well-formed UTF-8 byte sequences of the Unicode
 * Standard (the lead byte decides the length and, for E0, ED, F0 and F4, the
 * range of the second byte, which rejects overlong forms, surrogates and
 * values above U+10FFFF).  The bytes read never go past the NUL that ends
 * the text, because a NUL is not a valid continuation byte.
 */
static int
os_utf8_decode(
	const unsigned char *text,
	uint32_t *code,
	size_t *used)
{
	unsigned int lead;
	unsigned int second_min;
	unsigned int second_max;
	unsigned int next;
	size_t length;
	size_t i;
	uint32_t value;

	/* Takes the lead byte, which decides what the sequence must look like. */
	lead = text[0];

	/* An ASCII byte stands for itself. */
	if (lead < 0x80U) {
		*code = lead;
		*used = 1;
		return 0;
	}

	/* Finds the length, the first bits of the value and the range of the second byte. */
	second_min = 0x80U;
	second_max = 0xbfU;
	if (lead >= 0xc2U && lead <= 0xdfU) {
		length = 2;
		value = lead & 0x1fU;
	} else if (lead >= 0xe0U && lead <= 0xefU) {
		length = 3;
		value = lead & 0x0fU;
		if (lead == 0xe0U) {
			/* Below A0 the sequence would be an overlong form of a two-byte one. */
			second_min = 0xa0U;
		} else if (lead == 0xedU) {
			/* From A0 on the sequence would be a surrogate code point. */
			second_max = 0x9fU;
		}
	} else if (lead >= 0xf0U && lead <= 0xf4U) {
		length = 4;
		value = lead & 0x07U;
		if (lead == 0xf0U) {
			/* Below 90 the sequence would be an overlong form of a three-byte one. */
			second_min = 0x90U;
		} else if (lead == 0xf4U) {
			/* From 90 on the sequence would be above U+10FFFF. */
			second_max = 0x8fU;
		}
	} else {
		/* A lone continuation byte, C0, C1 and F5 to FF never start a sequence. */
		return EILSEQ;
	}

	/* Checks the second byte against its range. */
	next = text[1];
	if (next < second_min || next > second_max)
		return EILSEQ;
	value = (value << 6) | (next & 0x3fU);

	/* Checks the remaining bytes, each a continuation byte. */
	for (i = 2; i < length; i++) {
		next = text[i];
		if (next < 0x80U || next > 0xbfU)
			return EILSEQ;
		value = (value << 6) | (next & 0x3fU);
	}

	/* Hands the code point and the bytes it used to the caller. */
	*code = value;
	*used = length;

	/* Succeeded: one sequence was decoded. */
	return 0;
}

/*
 * Checks UTF-16 units and measures or writes the UTF-8 they make.
 *
 * With out NULL only the length is measured.  The length of the result is
 * kept within the layer's limit as it grows, so a huge input is refused
 * after at most that many bytes of work.
 */
static int
os_wide_encode(
	const wchar_t *wide,
	size_t count,
	char *out,
	size_t *bytes)
{
	size_t at;
	size_t written;
	size_t size;
	uint32_t unit;
	uint32_t next;
	uint32_t code;
	int stop_at_nul;

	/* A count of (size_t)-1 means the units run to the first NUL. */
	stop_at_nul = 0;
	if (count == (size_t)-1)
		stop_at_nul = 1;

	/* Walks the units one code point at a time. */
	at = 0;
	written = 0;
	for (;;) {
		/* Ends at the count when one was given. */
		if (stop_at_nul == 0 && at >= count)
			break;

		unit = (uint32_t)(uint16_t)wide[at];

		/* Ends at the NUL of a terminated string, but refuses one inside the count. */
		if (unit == 0) {
			if (stop_at_nul != 0)
				break;
			return EILSEQ;
		}

		if (unit >= 0xd800U && unit <= 0xdbffU) {
			/* A high surrogate needs a low surrogate right after it, inside the count. */
			if (stop_at_nul == 0 && at + 1U >= count)
				return EILSEQ;
			next = (uint32_t)(uint16_t)wide[at + 1U];
			if (next < 0xdc00U || next > 0xdfffU)
				return EILSEQ;
			code = OS_SUPPLEMENTARY_MIN + ((unit - 0xd800U) << 10) + (next - 0xdc00U);
			at += 2U;
		} else if (unit >= 0xdc00U && unit <= 0xdfffU) {
			/* A low surrogate without a high one before it is not text. */
			return EILSEQ;
		} else {
			code = unit;
			at++;
		}

		/* Finds how many bytes the code point takes. */
		if (code < 0x80U)
			size = 1;
		else if (code < 0x800U)
			size = 2;
		else if (code < OS_SUPPLEMENTARY_MIN)
			size = 3;
		else
			size = 4;

		/* Refuses a result longer than the layer accepts. */
		if (size > WB_OS_PATH_MAX - written)
			return ENAMETOOLONG;

		/* Writes the bytes of the code point when the caller gave a place for them. */
		if (out != NULL) {
			if (size == 1) {
				out[written] = (char)code;
			} else if (size == 2) {
				out[written] = (char)(0xc0U | (code >> 6));
				out[written + 1U] = (char)(0x80U | (code & 0x3fU));
			} else if (size == 3) {
				out[written] = (char)(0xe0U | (code >> 12));
				out[written + 1U] = (char)(0x80U | ((code >> 6) & 0x3fU));
				out[written + 2U] = (char)(0x80U | (code & 0x3fU));
			} else {
				out[written] = (char)(0xf0U | (code >> 18));
				out[written + 1U] = (char)(0x80U | ((code >> 12) & 0x3fU));
				out[written + 2U] = (char)(0x80U | ((code >> 6) & 0x3fU));
				out[written + 3U] = (char)(0x80U | (code & 0x3fU));
			}
		}

		written += size;
	}

	/* Hands the length of the UTF-8 to the caller. */
	*bytes = written;

	/* Succeeded: the units are valid text. */
	return 0;
}

/*
 * Asks the system for the full form of an absolute path.
 *
 * The answer is retried with a larger buffer when it did not fit, a bounded
 * number of times.
 */
static int
os_full_path(
	const wchar_t *source,
	wchar_t **full,
	size_t *full_length)
{
	wchar_t *buffer;
	DWORD size;
	DWORD got;
	int attempt;

	/* Asks how many units the full path needs, with its NUL. */
	size = GetFullPathNameW(source, 0, NULL, NULL);
	if (size == 0)
		return os_full_path_failure();

	/* Tries with the size the system named, and again if the answer grew. */
	for (attempt = 0; attempt < OS_FULL_PATH_ATTEMPTS; attempt++) {
		/* Allocates a buffer of the size asked for. */
		buffer = (wchar_t *)malloc((size_t)size * sizeof(wchar_t));
		if (buffer == NULL)
			return ENOMEM;

		/* Makes the full path; a result shorter than the buffer is the length without the NUL. */
		got = GetFullPathNameW(source, size, buffer, NULL);
		if (got == 0) {
			free(buffer);
			return os_full_path_failure();
		}
		if (got < size) {
			*full = buffer;
			*full_length = got;
			return 0;
		}

		/* The answer outgrew the buffer; takes the size it now asks for. */
		free(buffer);
		size = got;
	}

	/* The path kept changing size; gives up. */
	return EIO;
}

/*
 * Tells whether a path starts with the extended-length prefix "\\?\" or the
 * device prefix "\\.\", which the system takes verbatim.
 */
static int
os_path_is_extended(
	const wchar_t *path)
{
	if (wcsncmp(path, L"\\\\?\\", OS_VERBATIM_LENGTH) == 0)
		return 1;
	if (wcsncmp(path, L"\\\\.\\", OS_VERBATIM_LENGTH) == 0)
		return 1;

	/* Not a verbatim path. */
	return 0;
}

/*
 * Tells whether a path starts with a drive letter, a colon and a separator.
 */
static int
os_path_is_drive(
	const wchar_t *path)
{
	/* A drive letter is an ASCII letter. */
	if (!((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')))
		return 0;
	if (path[1] != L':')
		return 0;
	if (path[2] != L'\\')
		return 0;

	/* A drive path. */
	return 1;
}

/*
 * Turns the failure of GetFullPathNameW into the layer's error: a path that
 * is too long keeps its meaning, any other failure is EIO.
 */
static int
os_full_path_failure(
	void)
{
	DWORD code;
	int error;

	/* Takes the system's reason for the failure. */
	code = GetLastError();
	error = wb_os_error_from_system(code);

	/* Keeps the meaning of a path that is longer than the system accepts. */
	if (error == ENAMETOOLONG)
		return ENAMETOOLONG;

	/* Any other failure of the system. */
	return EIO;
}
