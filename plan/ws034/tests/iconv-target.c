/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Target smoke test of the C library's iconv (ws034-p040). */

#include <errno.h>
#include <iconv.h>
#include <stdio.h>
#include <string.h>

int
main(void)
{
	char input[] = "a\xc3\xa9\xe2\x82\xac!";
	char output[16];
	char *in = input, *out = output;
	size_t in_left = strlen(input), out_left = sizeof(output) - 1, value;
	iconv_t cd;

	cd = iconv_open("ASCII//TRANSLIT", "UTF-8");
	if (cd == (iconv_t)-1) {
		printf("ICONV FAIL open errno=%d\n", errno);
		return 1;
	}
	value = iconv(cd, &in, &in_left, &out, &out_left);
	*out = '\0';
	iconv_close(cd);
	errno = 0;
	if (iconv_open("ISO-8859-1", "UTF-8") != (iconv_t)-1 || errno != EINVAL) {
		printf("ICONV FAIL latin1 was accepted\n");
		return 1;
	}
	if (value != 2 || strcmp(output, "a?" "?!") != 0) {
		printf("ICONV FAIL value=%zu output=%s\n", value, output);
		return 1;
	}
	printf("ICONV PASS output=%s irreversible=%zu\n", output, value);
	return 0;
}
