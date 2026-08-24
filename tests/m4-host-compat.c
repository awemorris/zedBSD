/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <errno.h>
#include <limits.h>
#include <stdlib.h>

const char *
getprogname(void)
{
	return "m4";
}

long long
strtonum(const char *text, long long minimum, long long maximum,
	 const char **error_text)
{
	char *end;
	long long value;
	const char *error = NULL;

	errno = 0;
	value = strtoll(text, &end, 10);
	if (minimum > maximum || end == text || *end != '\0')
		error = "invalid";
	else if (errno == ERANGE || value < minimum)
		error = "too small";
	else if (value > maximum)
		error = "too large";
	if (error_text != NULL)
		*error_text = error;
	if (error != NULL) {
		errno = error[0] == 'i' ? EINVAL : ERANGE;
		return 0;
	}
	return value;
}
