/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <stdio.h>
#include <string.h>

char *crypt(const char *, const char *);

int
main(void)
{
	static const char expected[] =
	    "$6$saltsalt$qFmFH.bQmmtXzyBY0s9v7Oicd2z4XSIecDzlB5KiA2/"
	    "jctKu9YterLp8wwnSq.qc.eoxqOmSuNp2xS0ktL3nh/";
	char *actual = crypt("password", "$6$saltsalt$");
	if (actual == NULL || strcmp(actual, expected)) {
		fprintf(stderr, "SHA-512 crypt mismatch: %s\n",
		    actual != NULL ? actual : "(null)");
		return 1;
	}
	puts("crypt host test: PASS");
	return 0;
}
