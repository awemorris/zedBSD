/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The unit test of the errno texts (base/os-error.h): every value the
 * layer returns has the text the POSIX systems' strerror gives, the same
 * text on both systems (the Windows C runtime has none for several of
 * them), and an unlisted value gives "Unknown error".
 */

#include "check.h"

#include "base/os-error.h"

#include <errno.h>
#include <string.h>

/*
 * One errno value and the text it must have.
 */
struct expected_text {
	int code;
	const char *text;
};

/* The values the layer, the network and the TLS layer produce, with the texts read from glibc's strerror. */
static const struct expected_text expected_texts[] = {
	{ EINVAL, "Invalid argument" },
	{ ENOENT, "No such file or directory" },
	{ EEXIST, "File exists" },
	{ EACCES, "Permission denied" },
	{ EBUSY, "Device or resource busy" },
	{ ENOTDIR, "Not a directory" },
	{ ENAMETOOLONG, "File name too long" },
	{ ENOMEM, "Cannot allocate memory" },
	{ ENOSPC, "No space left on device" },
	{ ERANGE, "Numerical result out of range" },
	{ EILSEQ, "Invalid or incomplete multibyte or wide character" },
	{ ENOTSUP, "Operation not supported" },
	{ EAGAIN, "Resource temporarily unavailable" },
	{ EISDIR, "Is a directory" },
	{ ENOTEMPTY, "Directory not empty" },
	{ EMFILE, "Too many open files" },
	{ EFBIG, "File too large" },
	{ ETIMEDOUT, "Connection timed out" },
	{ EIO, "Input/output error" },
	{ EPROTO, "Protocol error" },
	{ EPROTONOSUPPORT, "Protocol not supported" },
	{ EOVERFLOW, "Value too large for defined data type" },
	{ EINPROGRESS, "Operation now in progress" },
	{ ECONNREFUSED, "Connection refused" },
	{ ECONNRESET, "Connection reset by peer" },
	{ ECONNABORTED, "Software caused connection abort" },
	{ ENETUNREACH, "Network is unreachable" },
	{ EHOSTUNREACH, "No route to host" },
	{ ENETDOWN, "Network is down" },
	{ EADDRNOTAVAIL, "Cannot assign requested address" },
	{ EAFNOSUPPORT, "Address family not supported by protocol" },
	{ EPIPE, "Broken pipe" },
	{ ENOBUFS, "No buffer space available" },
	{ EWOULDBLOCK, "Resource temporarily unavailable" },
	{ 0, "Success" }
};

/*
 * Checks every listed value, the unlisted ones and the text's lifetime.
 */
static void
test_texts(void)
{
	size_t index;
	const char *text;
	int different;

	/* Each value has its text. */
	for (index = 0; index < sizeof(expected_texts) / sizeof(expected_texts[0]); index++) {
		text = wb_os_error_text(expected_texts[index].code);
		CHECK(text != NULL);
		if (text != NULL) {
			different = strcmp(text, expected_texts[index].text);
			CHECK(different == 0);
			if (different != 0) {
				printf("value %d: \"%s\", expected \"%s\"\n", expected_texts[index].code, text,
					expected_texts[index].text);
			}
		}
	}

	/* A value no table lists is not a missing string. */
	text = wb_os_error_text(-1);
	CHECK(text != NULL);
	CHECK(strcmp(text, "Unknown error") == 0);
	text = wb_os_error_text(99999);
	CHECK(text != NULL);
	CHECK(strcmp(text, "Unknown error") == 0);

	/* The same value gives the same string (constant data). */
	CHECK(wb_os_error_text(EPROTO) == wb_os_error_text(EPROTO));
}

/*
 * Runs the checks and prints the summary.
 */
int
main(
	int argc,
	char **argv)
{
	int differs;
	int status;

	/* The harness passes the system's name. */
	CHECK(argc == 2);
	differs = 1;
	if (argc == 2)
		differs = strcmp(argv[1], "posix") != 0 && strcmp(argv[1], "win32") != 0;
	CHECK(differs == 0);

	/* The texts. */
	test_texts();

	/* Reports the summary line and the status. */
	status = check_finish("os-error");

	/* Succeeded when every check passed. */
	return status;
}
