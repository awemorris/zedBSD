/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The IPv6 probe's libc steps (ws130-p004, ipv6-probe -l [NAME]): an
 * address's text both ways (RFC 5952), an interface's name and index,
 * getaddrinfo of numeric IPv6 addresses (a link-local one with its zone),
 * of an IPv4 address as a v4-mapped one, of no node, and getnameinfo of an
 * IPv6 address with its zone.  With NAME, its addresses in the DNS (A and
 * AAAA) in getaddrinfo's order.  Each step prints "IPV6 libc STEP ok", or
 * "IPV6 FAIL step=STEP".
 */

#include "probe.h"

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

static int libc_text(void);
static int libc_interface(unsigned *ifindex);
static int libc_numeric(unsigned ifindex);
static int libc_mapped(void);
static int libc_unnamed(void);
static int libc_names(unsigned ifindex);
static int libc_lookup(const char *name);
static void libc_ok(const char *step);

/*
 * Runs the libc steps, and the DNS's for a name when one is given.
 * Returns 0 when every step passed.
 */
int
probe_libc(
	const char *name)
{
	unsigned ifindex;
	int status;

	/* Each step in turn; the first failure ends the run. */
	status = libc_text();
	if (status != 0)
		return status;
	status = libc_interface(&ifindex);
	if (status != 0)
		return status;
	status = libc_numeric(ifindex);
	if (status != 0)
		return status;
	status = libc_mapped();
	if (status != 0)
		return status;
	status = libc_unnamed();
	if (status != 0)
		return status;
	status = libc_names(ifindex);
	if (status != 0)
		return status;

	/* The name's addresses in the DNS. */
	if (name != NULL) {
		status = libc_lookup(name);
		if (status != 0)
			return status;
	}

	/* Succeeded. */
	return 0;
}

/* An address's text both ways: RFC 5952's form written, the long form read. */
static int
libc_text(void)
{
	struct in6_addr address;
	const char *written;
	char text[INET6_ADDRSTRLEN];
	int ok;
	int same;

	/* The long form read and written short. */
	ok = inet_pton(AF_INET6, "2001:0db8:0:0:1:0:0:1", &address);
	if (ok != 1)
		return probe_fail("libc-pton", EINVAL);
	written = inet_ntop(AF_INET6, &address, text, sizeof(text));
	if (written == NULL)
		return probe_fail("libc-ntop", errno);
	same = strcmp(text, "2001:db8::1:0:0:1");
	if (same != 0)
		return probe_fail("libc-ntop-form", EINVAL);

	/* A v4-mapped one keeps its dots. */
	ok = inet_pton(AF_INET6, "::FFFF:192.0.2.1", &address);
	if (ok != 1)
		return probe_fail("libc-pton-mapped", EINVAL);
	written = inet_ntop(AF_INET6, &address, text, sizeof(text));
	same = -1;
	if (written != NULL)
		same = strcmp(text, "::ffff:192.0.2.1");
	if (same != 0)
		return probe_fail("libc-ntop-mapped", EINVAL);

	/* Not an address. */
	ok = inet_pton(AF_INET6, "1:::2", &address);
	if (ok != 0)
		return probe_fail("libc-pton-invalid", EINVAL);

	/* Succeeded. */
	libc_ok("text");
	return 0;
}

/* The loopback's index from its name, and its name back. */
static int
libc_interface(
	unsigned *ifindex)
{
	char name[IF_NAMESIZE];
	const char *found;
	int same;

	/* The index. */
	*ifindex = if_nametoindex("lo0");
	if (*ifindex == 0U)
		return probe_fail("libc-nametoindex", errno);

	/* The name back. */
	found = if_indextoname(*ifindex, name);
	same = -1;
	if (found != NULL)
		same = strcmp(name, "lo0");
	if (same != 0)
		return probe_fail("libc-indextoname", errno);

	/* Succeeded. */
	printf("IPV6 libc interface lo0 index=%u\n", *ifindex);
	libc_ok("interface");
	return 0;
}

/* Numeric IPv6 addresses: ::1 with a port, a link-local one with its zone, and ::1 refused for AF_INET. */
static int
libc_numeric(
	unsigned ifindex)
{
	struct addrinfo hints;
	struct addrinfo *found;
	const struct sockaddr_in6 *address;
	int error;
	int good;

	/* ::1 and a port, one record. */
	memset(&hints, 0, sizeof(hints));
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_NUMERICHOST;
	error = getaddrinfo("::1", "80", &hints, &found);
	if (error != 0)
		return probe_fail("libc-gai-loopback", error);
	address = (const struct sockaddr_in6 *)found->ai_addr;
	good = found->ai_family == AF_INET6 && found->ai_next == NULL;
	if (good)
		good = ntohs(address->sin6_port) == 80U && IN6_IS_ADDR_LOOPBACK(&address->sin6_addr);
	freeaddrinfo(found);
	if (!good)
		return probe_fail("libc-gai-loopback-record", EINVAL);

	/* A link-local address with its interface's name: the scope is its index. */
	error = getaddrinfo("fe80::1%lo0", NULL, &hints, &found);
	if (error != 0)
		return probe_fail("libc-gai-zone", error);
	address = (const struct sockaddr_in6 *)found->ai_addr;
	good = address->sin6_scope_id == ifindex;
	freeaddrinfo(found);
	if (!good)
		return probe_fail("libc-gai-zone-scope", EINVAL);

	/* ::1 is not of AF_INET. */
	hints.ai_family = AF_INET;
	error = getaddrinfo("::1", NULL, &hints, &found);
	if (error == 0) {
		freeaddrinfo(found);
		return probe_fail("libc-gai-family", EINVAL);
	}

	/* Succeeded. */
	libc_ok("numeric");
	return 0;
}

/* An IPv4 address asked for as AF_INET6 with AI_V4MAPPED: ::ffff:127.0.0.1. */
static int
libc_mapped(void)
{
	struct addrinfo hints;
	struct addrinfo *found;
	const struct sockaddr_in6 *address;
	const char *written;
	char text[INET6_ADDRSTRLEN];
	int error;
	int same;

	/* The question. */
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET6;
	hints.ai_flags = AI_V4MAPPED;
	error = getaddrinfo("127.0.0.1", NULL, &hints, &found);
	if (error != 0)
		return probe_fail("libc-gai-mapped", error);

	/* The answer as text. */
	address = (const struct sockaddr_in6 *)found->ai_addr;
	written = inet_ntop(AF_INET6, &address->sin6_addr, text, sizeof(text));
	same = -1;
	if (written != NULL)
		same = strcmp(text, "::ffff:127.0.0.1");
	freeaddrinfo(found);
	if (same != 0)
		return probe_fail("libc-gai-mapped-address", EINVAL);

	/* Succeeded. */
	libc_ok("mapped");
	return 0;
}

/* No node, passive, either family: 0.0.0.0, then ::. */
static int
libc_unnamed(void)
{
	struct addrinfo hints;
	struct addrinfo *found;
	int good;
	int error;

	/* The question. */
	memset(&hints, 0, sizeof(hints));
	hints.ai_flags = AI_PASSIVE;
	hints.ai_socktype = SOCK_DGRAM;
	error = getaddrinfo(NULL, "7", &hints, &found);
	if (error != 0)
		return probe_fail("libc-gai-passive", error);

	/* IPv4's, then IPv6's. */
	good = found->ai_family == AF_INET && found->ai_next != NULL;
	if (good)
		good = found->ai_next->ai_family == AF_INET6;
	freeaddrinfo(found);
	if (!good)
		return probe_fail("libc-gai-passive-order", EINVAL);

	/* Succeeded. */
	libc_ok("unnamed");
	return 0;
}

/* getnameinfo of a link-local address with its zone: by the interface's name, and by its number with NI_NUMERICSCOPE. */
static int
libc_names(
	unsigned ifindex)
{
	struct sockaddr_in6 address;
	char host[NI_MAXHOST];
	char service[NI_MAXSERV];
	char numbered[64];
	int error;
	int same;

	/* fe80::1 on the loopback, port 53. */
	memset(&address, 0, sizeof(address));
	address.sin6_family = AF_INET6;
	address.sin6_port = htons(53U);
	(void)inet_pton(AF_INET6, "fe80::1", &address.sin6_addr);
	address.sin6_scope_id = ifindex;

	/* By the interface's name. */
	error = getnameinfo((const struct sockaddr *)&address, sizeof(address), host, sizeof(host), service, sizeof(service), NI_NUMERICHOST | NI_NUMERICSERV);
	if (error != 0)
		return probe_fail("libc-gni", error);
	same = strcmp(host, "fe80::1%lo0");
	if (same == 0)
		same = strcmp(service, "53");
	if (same != 0)
		return probe_fail("libc-gni-text", EINVAL);

	/* By its number. */
	error = getnameinfo((const struct sockaddr *)&address, sizeof(address), host, sizeof(host), NULL, 0, NI_NUMERICHOST | NI_NUMERICSCOPE);
	if (error != 0)
		return probe_fail("libc-gni-scope", error);
	(void)snprintf(numbered, sizeof(numbered), "fe80::1%%%u", ifindex);
	same = strcmp(host, numbered);
	if (same != 0)
		return probe_fail("libc-gni-scope-text", EINVAL);

	/* Succeeded. */
	libc_ok("names");
	return 0;
}

/* A name's addresses in the DNS, in getaddrinfo's order; at least one. */
static int
libc_lookup(
	const char *name)
{
	struct addrinfo hints;
	struct addrinfo *found;
	struct addrinfo *item;
	const char *family;
	char host[NI_MAXHOST];
	int error;
	int count;

	/* The question, either family. */
	memset(&hints, 0, sizeof(hints));
	hints.ai_socktype = SOCK_STREAM;
	error = getaddrinfo(name, "443", &hints, &found);
	if (error != 0)
		return probe_fail("libc-lookup", error);

	/* Each answer. */
	count = 0;
	for (item = found; item != NULL; item = item->ai_next) {
		error = getnameinfo(item->ai_addr, item->ai_addrlen, host, sizeof(host), NULL, 0, NI_NUMERICHOST);
		if (error != 0)
			continue;
		family = "inet";
		if (item->ai_family == AF_INET6)
			family = "inet6";
		printf("IPV6 libc lookup %s family=%s address=%s\n", name, family, host);
		count++;
	}

	/* At least one. */
	freeaddrinfo(found);
	if (count == 0)
		return probe_fail("libc-lookup-empty", ENOENT);

	/* Succeeded. */
	libc_ok("lookup");
	return 0;
}

/* Prints a step that passed. */
static void
libc_ok(
	const char *step)
{
	/* The line the test reads. */
	printf("IPV6 libc %s ok\n", step);
}
