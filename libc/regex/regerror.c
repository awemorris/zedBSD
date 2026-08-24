/* Derived from musl libc; SPDX-License-Identifier: MIT */
#include <regex.h>
#include <stdio.h>
#include <string.h>

static const char messages[] = {"No error\0"
				"No match\0"
				"Invalid regexp\0"
				"Unknown collating element\0"
				"Unknown character class name\0"
				"Trailing backslash\0"
				"Invalid back reference\0"
				"Missing ']'\0"
				"Missing ')'\0"
				"Missing '}'\0"
				"Invalid contents of {}\0"
				"Invalid character range\0"
				"Out of memory\0"
				"Repetition not preceded by valid expression\0"
				"\0Unknown error"};

size_t
regerror(int error, const regex_t *restrict expression, char *restrict buffer,
	 size_t capacity)
{
	const char *message;

	(void)expression;
	for (message = messages; error != 0 && *message != '\0'; error--)
		message += strlen(message) + 1;
	if (*message == '\0')
		message++;
	return 1 + (size_t)snprintf(buffer, capacity, "%s", message);
}
