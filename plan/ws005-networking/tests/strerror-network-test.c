/* WS005 production libc network error mapping fixture. SPDX-License-Identifier:
 * Zlib */
#include <errno.h>
#include <stddef.h>
#include <string.h>

/* The host C runtime references strdup during startup on some toolchains,
 * retaining that production section despite --gc-sections. */
char *
heap_strdup_active(const char *string)
{
	(void)string;
	return NULL;
}

int
main(void)
{
	if (strcmp(strerror(ENETDOWN), "Network is down") != 0)
		return 1;
	if (strcmp(strerror(ETIMEDOUT), "Connection timed out") != 0)
		return 1;
	return 0;
}
