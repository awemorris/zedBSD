/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws177-p022: a resolver that does not answer for "slow.test" (30 s, then
 * no such name), preloaded into keiland-printd on the host so that its ten
 * seconds' bound on a look up can be seen.  Every other name goes to the
 * system's getaddrinfo.
 */

#define _GNU_SOURCE

#include <dlfcn.h>
#include <netdb.h>
#include <string.h>
#include <unistd.h>

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **result);

/* Looks a name up: "slow.test" after 30 s with no answer, the others by the system. */
int
getaddrinfo(
	const char *node,
	const char *service,
	const struct addrinfo *hints,
	struct addrinfo **result)
{
	int (*real)(const char *, const char *, const struct addrinfo *, struct addrinfo **);
	int same;
	int found;

	/* The name that stands still. */
	same = 1;
	if (node != NULL)
		same = strcmp(node, "slow.test");
	if (same == 0 && (hints == NULL || (hints->ai_flags & AI_NUMERICHOST) == 0)) {
		sleep(30);
		return EAI_NONAME;
	}

	/* The system's. */
	real = (int (*)(const char *, const char *, const struct addrinfo *, struct addrinfo **))dlsym(RTLD_NEXT, "getaddrinfo");
	found = real(node, service, hints, result);
	return found;
}
