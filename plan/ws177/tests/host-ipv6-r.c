/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of case R of WS177 (ws177-p044 to p046), built with the
 * host's headers:
 *
 *   libc   which interface addresses count for AI_ADDRCONFIG (IPv4: not
 *          the unspecified nor loopback's; IPv6: settled, not ::, ::1
 *          nor link-local) and a host name cut for NI_NOFQDN
 *          (userland/base/libc/resolver-dns.c)
 *
 *   plan/ws177/tests/host-ipv6-r.sh
 */

#include "userland/base/libc/resolver-internal.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

/* The checks that failed, and those that held. */
static int failures;
static int passes;

static void test_libc(void);
static int usable6(const char *text, int settled);
static void check(int condition, const char *what);

/*
 * Runs the groups; the exit status says whether every check held.
 */
int
main(void)
{
	/* The groups. */
	test_libc();

	/* The verdict. */
	printf("host-ipv6-r: %d passed, %d failed\n", passes, failures);
	if (failures != 0)
		return 1;

	/* Succeeded: every check held. */
	return 0;
}

/* AI_ADDRCONFIG's addresses and NI_NOFQDN's name (ws177-p044). */
static void
test_libc(void)
{
	char name[64];

	/* IPv4: a configured address counts, the unspecified one and loopback's do not. */
	check(resolver_usable4(0xc0a80002U), "libc: 192.168.0.2 counts");
	check(!resolver_usable4(0U), "libc: 0.0.0.0 does not");
	check(!resolver_usable4(0x7f000001U) && !resolver_usable4(0x7f0000feU), "libc: 127.0.0.0/8 does not");

	/* IPv6: a settled global or unique local address counts. */
	check(usable6("2001:db8::5", 1), "libc: 2001:db8::5 counts");
	check(usable6("fd00::1", 1), "libc: a unique local address counts");
	check(!usable6("2001:db8::5", 0), "libc: one still tentative (or duplicated) does not");
	check(!usable6("::1", 1) && !usable6("::", 1), "libc: ::1 and :: do not");
	check(!usable6("fe80::1", 1) && !usable6("febf::1", 1), "libc: link-local ones do not");

	/* NI_NOFQDN: the first label. */
	(void)snprintf(name, sizeof(name), "%s", "host.example.org");
	resolver_short_name(name);
	check(strcmp(name, "host") == 0, "libc: host.example.org is host");
	(void)snprintf(name, sizeof(name), "%s", "localhost");
	resolver_short_name(name);
	check(strcmp(name, "localhost") == 0, "libc: a name without a dot stays");
	(void)snprintf(name, sizeof(name), "%s", ".odd");
	resolver_short_name(name);
	check(strcmp(name, ".odd") == 0, "libc: a dot first leaves it");
}

/* Tells whether an IPv6 address in text counts for AI_ADDRCONFIG. */
static int
usable6(
	const char *text,
	int settled)
{
	struct in6_addr address;
	int parsed;
	int usable;

	/* The address. */
	parsed = inet_pton(AF_INET6, text, &address);
	if (parsed != 1)
		return -1;

	/* Whether it counts. */
	usable = resolver_usable6(address.s6_addr, settled);
	return usable;
}

/* Counts one check and prints it. */
static void
check(
	int condition,
	const char *what)
{
	/* Held. */
	if (condition) {
		passes++;
		printf("ok %s\n", what);
		return;
	}

	/* Failed. */
	failures++;
	printf("FAILED %s\n", what);
}
