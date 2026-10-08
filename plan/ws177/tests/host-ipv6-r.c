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
 *   slaac  a temporary address's lifetimes counted from when it was made,
 *          when the next one is made, the desynchronization, and the
 *          router chosen for the default route
 *          (userland/base/networkd/slaac.c, ws177-p045)
 *   dns    the RDNSS servers merged into resolv.conf after another writer
 *          (userland/base/networkd/resolver6.c, ws177-p045)
 *   net    the static IPv6 addresses and IPv6 routes net commit takes
 *          away (userland/base/net/reconcile.c, ws177-p045)
 *   dhcp6  Rebind, Release and Decline built, T1 and T2, Renew or Rebind
 *          or Solicit by the lease's age (userland/base/net/dhcp6.c,
 *          ws177-p046)
 *   names  a DNSSL's or a Domain Search List's name with a label that is
 *          not a host name's dropped whole (slaac.c, dhcp6.c, ws177-p046)
 *
 *   plan/ws177/tests/host-ipv6-r.sh
 */

#include "userland/base/libc/resolver-internal.h"
#include "userland/base/net/dhcp6.h"
#include "userland/base/net/netconf.h"
#include "userland/base/net/reconcile.h"
#include "userland/base/networkd/resolver6.h"
#include "userland/base/networkd/slaac.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

/* The checks that failed, and those that held. */
static int failures;
static int passes;

/* The operations a reconcile emitted, one a line. */
static char program[4096];

/* A net.conf before a commit: two static IPv6 addresses on ue0, and two IPv6 routes. */
static const char net_before[] =
	"version: 1\n"
	"interfaces:\n"
	"  ue0:\n"
	"    type: ethernet\n"
	"    enabled: true\n"
	"    ipv6:\n"
	"      addresses:\n"
	"        - address: 2001:db8::10\n"
	"          prefix-length: 64\n"
	"        - address: 2001:db8::20\n"
	"          prefix-length: 64\n"
	"routes:\n"
	"  - destination: 2001:db8:1::/48\n"
	"    gateway: fe80::1\n"
	"    interface: ue0\n"
	"  - destination: 2001:db8:2::/48\n"
	"    gateway: fe80::1\n"
	"    interface: ue0\n"
	"dns:\n"
	"  mode: dhcp\n";

/* After: one address left (written another way), one route left. */
static const char net_after[] =
	"version: 1\n"
	"interfaces:\n"
	"  ue0:\n"
	"    type: ethernet\n"
	"    enabled: true\n"
	"    ipv6:\n"
	"      addresses:\n"
	"        - address: 2001:0db8:0::10\n"
	"          prefix-length: 64\n"
	"routes:\n"
	"  - destination: 2001:db8:2::/48\n"
	"    gateway: fe80::2\n"
	"    interface: ue0\n"
	"dns:\n"
	"  mode: dhcp\n";

/* After, with ue0's IPv6 off. */
static const char net_off[] =
	"version: 1\n"
	"interfaces:\n"
	"  ue0:\n"
	"    type: ethernet\n"
	"    enabled: true\n"
	"    ipv6:\n"
	"      enabled: false\n"
	"dns:\n"
	"  mode: dhcp\n";

static void test_libc(void);
static void test_slaac(void);
static void test_dns(void);
static void test_net(void);
static void test_dhcp6(void);
static void test_names(void);
static const uint8_t *option_find(const uint8_t *message, size_t length, unsigned code, size_t *size);
static size_t names(uint8_t *bytes, const char *const *list, unsigned count);
static int usable6(const char *text, int settled);
static int merge(const char *current, const char *owned, const char *wanted, const char *search, char *output, size_t capacity, struct resolver6_servers *now_owned);
static void servers(const char *list, struct resolver6_servers *result);
static int reconcile(const char *before, const char *after);
static int parse(const char *text, struct netconf *configuration);
static int record(const char *operation, const char *operands, void *context);
static void check(int condition, const char *what);

/*
 * Runs the groups; the exit status says whether every check held.
 */
int
main(void)
{
	/* The groups. */
	test_libc();
	test_slaac();
	test_dns();
	test_net();
	test_dhcp6();
	test_names();

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

/* A temporary address's lifetimes, its next one, its desynchronization, and the router chosen (ws177-p045). */
static void
test_slaac(void)
{
	struct slaac_router routers[4];
	uint32_t valid;
	uint32_t preferred;
	uint64_t regenerate;
	uint32_t desync;
	int chosen;

	/* New: two days valid, a day less the desynchronization preferred. */
	slaac_temporary_aged(7U * 86400U, 7U * 86400U, 1000U, 0U, &valid, &preferred);
	check(valid == 2U * 86400U && preferred == 86400U - 1000U, "slaac: a new temporary address's lifetimes");

	/* Aged: what is left from when it was made, not renewed by the advertisement. */
	slaac_temporary_aged(7U * 86400U, 7U * 86400U, 1000U, 86400U - 1010U, &valid, &preferred);
	check(valid == 2U * 86400U - (86400U - 1010U) && preferred == 10U, "slaac: aged, 10 seconds preferred left");
	slaac_temporary_aged(7U * 86400U, 7U * 86400U, 1000U, 86400U, &valid, &preferred);
	check(valid == 86400U && preferred == 0U, "slaac: past its preferred time, deprecated");
	slaac_temporary_aged(7U * 86400U, 7U * 86400U, 1000U, 3U * 86400U, &valid, &preferred);
	check(valid == 0U && preferred == 0U, "slaac: past two days, gone");

	/* The prefix's shorter lifetimes win. */
	slaac_temporary_aged(3600U, 1800U, 1000U, 100U, &valid, &preferred);
	check(valid == 3600U && preferred == 1800U, "slaac: the prefix's lifetimes when shorter");

	/* The next one: REGEN_ADVANCE before the longest preferred time; the desynchronization's range. */
	regenerate = slaac_temporary_regenerate(1000U);
	check(regenerate == 86400U - 1000U - 5U, "slaac: the next temporary address 5 seconds before");
	desync = slaac_temporary_desync(34560U);
	check(desync == 34560U, "slaac: MAX_DESYNC_FACTOR is in range");
	desync = slaac_temporary_desync(34561U);
	check(desync == 0U, "slaac: past it wraps");
	desync = slaac_temporary_desync(0xffffffffU);
	check(desync <= SLAAC_TEMPORARY_DESYNC_MAX, "slaac: any random number in range");

	/* The router: the best rank alive, the current one among equals. */
	memset(routers, 0, sizeof(routers));
	routers[0].used = 1;
	routers[0].rank = 17U;
	routers[0].expires = 1000U;
	routers[2].used = 1;
	routers[2].rank = 0U;
	routers[2].expires = 1000U;
	chosen = slaac_router_choose(routers, 4U, 10U, -1);
	check(chosen == 2, "slaac: the wired router before Wi-Fi's");
	chosen = slaac_router_choose(routers, 4U, 1000U, -1);
	check(chosen == -1, "slaac: none when every lifetime ended");
	routers[2].expires = 5U;
	chosen = slaac_router_choose(routers, 4U, 10U, 2);
	check(chosen == 0, "slaac: another when the chosen one's lifetime ended");
	routers[2].expires = 1000U;
	routers[3].used = 1;
	routers[3].rank = 0U;
	routers[3].expires = 1000U;
	chosen = slaac_router_choose(routers, 4U, 10U, 3);
	check(chosen == 3, "slaac: the current router among equals");
	chosen = slaac_router_choose(routers, 4U, 10U, -1);
	check(chosen == 2, "slaac: else the first among equals");
}

/* The RDNSS servers merged into resolv.conf (ws177-p045). */
static void
test_dns(void)
{
	struct resolver6_servers owned;
	char output[1024];
	char tiny[16];
	int status;

	/* DHCPv4 rewrote the file: the RDNSS server back after its own, its first line kept. */
	status = merge("# Generated by dhcpc for em0\nsearch example.org\nnameserver 10.0.2.3\n", "2001:db8::53", "2001:db8::53", "",
	    output, sizeof(output), &owned);
	check(status == 1 && strcmp(output, "# Generated by dhcpc for em0\nsearch example.org\nnameserver 10.0.2.3\n"
	    "nameserver 2001:db8::53\n") == 0, "dns: put back after DHCPv4's rewrite, the dhcpc line kept");
	check(owned.count == 1U && strcmp(owned.text[0], "2001:db8::53") == 0, "dns: networkd's own");

	/* Already there: nothing to write. */
	status = merge(output, "2001:db8::53", "2001:db8::53", "", output, sizeof(output), &owned);
	check(status == 0 && owned.count == 1U, "dns: no change, no write");

	/* Its lifetime ended: taken out, the rest kept. */
	status = merge("# Generated by dhcpc for em0\nnameserver 10.0.2.3\nnameserver 2001:db8::53\noptions ndots:2\n", "2001:db8::53",
	    "", "", output, sizeof(output), &owned);
	check(status == 1 && strcmp(output, "# Generated by dhcpc for em0\noptions ndots:2\nnameserver 10.0.2.3\n") == 0,
	    "dns: one no longer wanted taken out");
	check(owned.count == 0U, "dns: none networkd's left");

	/* The same server another writer put there (dhcpc -6) is not networkd's, and stays. */
	status = merge("# Generated by dhcpc -6 for em0\nnameserver 2001:db8::53\n", "", "2001:db8::53", "", output, sizeof(output),
	    &owned);
	check(status == 0 && owned.count == 0U, "dns: another writer's server stays its own");

	/* Three servers already: no room, no change. */
	status = merge("nameserver 10.0.0.1\nnameserver 10.0.0.2\nnameserver 10.0.0.3\n", "", "2001:db8::53", "", output,
	    sizeof(output), &owned);
	check(status == 0 && owned.count == 0U, "dns: no fourth server");

	/* No file: networkd's first line, the search list, a link-local server with its zone. */
	status = merge("", "", "fe80::1%em0", "example.org", output, sizeof(output), &owned);
	check(status == 1 && strcmp(output, "# Generated by networkd\nsearch example.org\nnameserver fe80::1%em0\n") == 0,
	    "dns: a new file");

	/* A file without a first comment gets networkd's; a search list there is not replaced. */
	status = merge("search a.example\nnameserver 10.0.0.1\n", "", "2001:db8::53", "b.example", output, sizeof(output), &owned);
	check(status == 1 && strcmp(output, "# Generated by networkd\nsearch a.example\nnameserver 10.0.0.1\n"
	    "nameserver 2001:db8::53\n") == 0, "dns: the search list there kept");

	/* A file without a final newline, and an output too small. */
	status = merge("nameserver 10.0.0.1", "", "2001:db8::53", "", output, sizeof(output), &owned);
	check(status == 1 && strcmp(output, "# Generated by networkd\nnameserver 10.0.0.1\nnameserver 2001:db8::53\n") == 0,
	    "dns: the last line without a newline");
	status = merge("nameserver 10.0.0.1\n", "", "2001:db8::53", "", tiny, sizeof(tiny), &owned);
	check(status == -1, "dns: an output too small is refused");
}

/* The static IPv6 addresses and IPv6 routes net commit takes away (ws177-p045). */
static void
test_net(void)
{
	int status;

	/* One address and one route dropped: taken away first; the address written another way stays. */
	status = reconcile(net_before, net_after);
	check(status == 0, "net: reconciled");
	check(strstr(program, "STATIC6_REMOVE ue0 2001:db8::20/64\n") != NULL, "net: the dropped address taken away");
	check(strstr(program, "STATIC6_REMOVE ue0 2001:db8::10/64") == NULL, "net: the kept address stays");
	check(strstr(program, "ROUTE6_REMOVE 2001:db8:1::/48\n") != NULL, "net: the dropped route taken away");
	check(strstr(program, "ROUTE6_REMOVE 2001:db8:2::/48") == NULL, "net: the route named again is replaced, not taken away");
	check(strstr(program, "ROUTE6_REMOVE") < strstr(program, "UP ue0"), "net: before the interface is set up");

	/* IPv6 turned off: no address one by one (turning it off takes them). */
	status = reconcile(net_before, net_off);
	check(status == 0 && strstr(program, "STATIC6_REMOVE") == NULL, "net: none with IPv6 off");
	check(strstr(program, "ROUTE6_REMOVE 2001:db8:1::/48\n") != NULL, "net: the routes still taken away");

	/* Nothing changed: nothing taken away. */
	status = reconcile(net_before, net_before);
	check(status == 0 && strstr(program, "_REMOVE") == NULL, "net: none for the same file");
}

/* Rebind, Release and Decline, T1 and T2, and what a lease's age calls for (ws177-p046). */
static void
test_dhcp6(void)
{
	struct dhcp6_request request;
	struct dhcp6_reply reply;
	struct dhcp6_duid client;
	struct dhcp6_duid server;
	uint8_t random[16];
	uint8_t message[512];
	const uint8_t *ia;
	size_t length;
	size_t size;
	uint32_t t1;
	uint32_t t2;
	unsigned next;
	int status;

	/* The identifiers. */
	memset(random, 0x5a, sizeof(random));
	dhcp6_duid_uuid(random, &client);
	memset(random, 0xa5, sizeof(random));
	dhcp6_duid_uuid(random, &server);

	/* A Rebind: no server's identifier, the IA_NA with the address, the options asked for. */
	memset(&request, 0, sizeof(request));
	request.type = DHCP6_REBIND;
	request.xid = 0x123456U;
	request.client = &client;
	request.with_ia = 1;
	request.with_address = 1;
	(void)inet_pton(AF_INET6, "fd00:6::150", &request.address);
	status = dhcp6_build(message, sizeof(message), &length, &request);
	check(status == 0 && message[0] == DHCP6_REBIND, "dhcp6: a Rebind built");
	check(option_find(message, length, 2U, &size) == NULL, "dhcp6: a Rebind names no server");
	ia = option_find(message, length, 3U, &size);
	check(ia != NULL && size == 12U + 4U + 24U && memcmp(ia + 16, request.address.s6_addr, 16U) == 0,
	    "dhcp6: a Rebind's IA_NA has the address");
	check(option_find(message, length, 6U, &size) != NULL, "dhcp6: a Rebind asks for options");

	/* A Release and a Decline: the server's identifier, the address, no options asked for. */
	request.type = DHCP6_RELEASE;
	request.server = &server;
	status = dhcp6_build(message, sizeof(message), &length, &request);
	check(status == 0 && message[0] == DHCP6_RELEASE, "dhcp6: a Release built");
	check(option_find(message, length, 2U, &size) != NULL && size == server.length, "dhcp6: a Release names the server");
	check(option_find(message, length, 6U, &size) == NULL, "dhcp6: a Release asks for no options");
	request.type = DHCP6_DECLINE;
	status = dhcp6_build(message, sizeof(message), &length, &request);
	check(status == 0 && message[0] == DHCP6_DECLINE, "dhcp6: a Decline built");
	request.with_address = 0;
	status = dhcp6_build(message, sizeof(message), &length, &request);
	check(status != 0, "dhcp6: a Decline without its address is refused");
	request.type = DHCP6_RELEASE;
	request.with_address = 1;
	request.server = NULL;
	status = dhcp6_build(message, sizeof(message), &length, &request);
	check(status != 0, "dhcp6: a Release without the server is refused");

	/* T1 and T2: the server's; else half and four fifths of the preferred lifetime. */
	memset(&reply, 0, sizeof(reply));
	reply.t1 = 60U;
	reply.t2 = 96U;
	reply.preferred = 120U;
	dhcp6_lease_times(&reply, &t1, &t2);
	check(t1 == 60U && t2 == 96U, "dhcp6: the server's T1 and T2");
	reply.t1 = 0U;
	reply.t2 = 0U;
	dhcp6_lease_times(&reply, &t1, &t2);
	check(t1 == 60U && t2 == 96U, "dhcp6: T1 and T2 left to the client");
	reply.t1 = 100U;
	reply.t2 = 50U;
	dhcp6_lease_times(&reply, &t1, &t2);
	check(t1 == 60U && t2 == 60U, "dhcp6: a T1 past T2 taken from the lifetime, T2 not before it");
	reply.t1 = 0U;
	reply.t2 = 0U;
	reply.preferred = DHCP6_INFINITE;
	dhcp6_lease_times(&reply, &t1, &t2);
	check(t1 == DHCP6_INFINITE && t2 == DHCP6_INFINITE, "dhcp6: an infinite lifetime, no renewal");

	/* What the lease's age calls for. */
	next = dhcp6_lease_next(30U, 96U, 120U);
	check(next == DHCP6_RENEW, "dhcp6: a Renew before T2");
	next = dhcp6_lease_next(96U, 96U, 120U);
	check(next == DHCP6_REBIND, "dhcp6: a Rebind from T2");
	next = dhcp6_lease_next(120U, 96U, 120U);
	check(next == DHCP6_SOLICIT, "dhcp6: a Solicit once the lease is over");
	next = dhcp6_lease_next(1000000U, DHCP6_INFINITE, DHCP6_INFINITE);
	check(next == DHCP6_RENEW, "dhcp6: a record without times: a Renew");
}

/* A name with a bad label dropped whole, in an RA's DNSSL and a DHCPv6 Domain Search List (ws177-p046). */
static void
test_names(void)
{
	static const char *const list[] = { "good.example", "bad.ex ample", "next.example" };
	static const char *const tail[] = { "first.example", "half.b@d" };
	struct dhcp6_reply reply;
	struct slaac_ra ra;
	uint8_t message[256];
	uint8_t encoded[128];
	size_t encoded_length;
	size_t option_length;
	size_t length;
	int status;

	/* An RA with a DNSSL: the good names kept, the bad one gone whole, the one after it kept. */
	memset(message, 0, sizeof(message));
	message[0] = 134U;
	encoded_length = names(encoded, list, 3U);
	option_length = (8U + encoded_length + 7U) / 8U * 8U;
	message[16] = 31U;
	message[17] = (uint8_t)(option_length / 8U);
	message[23] = 60U;
	memcpy(message + 24, encoded, encoded_length);
	length = 16U + option_length;
	status = slaac_parse(message, length, &ra);
	check(status == 0 && strcmp(ra.search, "good.example next.example") == 0, "names: a DNSSL's bad name dropped whole");
	check(ra.search_lifetime == 60U, "names: the DNSSL's lifetime");

	/* Two DNSSL options: their names apart. */
	memcpy(message + 16U + option_length, message + 16, option_length);
	length = 16U + option_length * 2U;
	status = slaac_parse(message, length, &ra);
	check(status == 0 && strcmp(ra.search, "good.example next.example good.example next.example") == 0,
	    "names: two DNSSL options' names apart");

	/* A DHCPv6 Reply's Domain Search List: the last name's bad half not kept. */
	memset(message, 0, sizeof(message));
	message[0] = DHCP6_REPLY;
	message[3] = 0x01U;
	encoded_length = names(encoded, tail, 2U);
	message[4] = 0U;
	message[5] = 24U;
	message[6] = 0U;
	message[7] = (uint8_t)encoded_length;
	memcpy(message + 8, encoded, encoded_length);
	length = 8U + encoded_length;
	status = dhcp6_parse(message, length, 0x01U, NULL, &reply);
	check(status == 0 && strcmp(reply.search, "first.example") == 0, "names: a Domain Search List's bad name dropped whole");
}

/* Finds an option of a DHCPv6 message (after its four bytes of type and transaction); NULL when it has none. */
static const uint8_t *
option_find(
	const uint8_t *message,
	size_t length,
	unsigned code,
	size_t *size)
{
	size_t offset;
	unsigned found;

	/* Each option. */
	offset = 4U;
	while (offset + 4U <= length) {
		found = (unsigned)message[offset] << 8 | message[offset + 1U];
		*size = (size_t)message[offset + 2U] << 8 | message[offset + 3U];
		if (found == code)
			return message + offset + 4U;
		offset += 4U + *size;
	}

	/* None. */
	return NULL;
}

/* Writes names in DNS's label form (a space inside a label is kept as it is); the bytes written. */
static size_t
names(
	uint8_t *bytes,
	const char *const *list,
	unsigned count)
{
	const char *label;
	size_t used;
	size_t size;
	unsigned index;

	/* Each name's labels, then a zero. */
	used = 0;
	for (index = 0; index < count; index++) {
		label = list[index];
		while (*label != '\0') {
			size = strcspn(label, ".");
			bytes[used++] = (uint8_t)size;
			memcpy(bytes + used, label, size);
			used += size;
			label += size;
			if (*label == '.')
				label++;
		}

		/* The name's end. */
		bytes[used++] = 0U;
	}

	/* The length. */
	return used;
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

/* Merges with lists given as text, the servers separated by spaces. */
static int
merge(
	const char *current,
	const char *owned,
	const char *wanted,
	const char *search,
	char *output,
	size_t capacity,
	struct resolver6_servers *now_owned)
{
	struct resolver6_servers owned_list;
	struct resolver6_servers wanted_list;
	char copy[1024];
	int status;

	/* The lists, and the current text copied (the output may be the same buffer). */
	servers(owned, &owned_list);
	servers(wanted, &wanted_list);
	(void)snprintf(copy, sizeof(copy), "%s", current);

	/* The merge. */
	status = resolver6_merge(copy, strlen(copy), &owned_list, &wanted_list, search, output, capacity, now_owned);
	return status;
}

/* Reads a list of servers separated by spaces. */
static void
servers(
	const char *list,
	struct resolver6_servers *result)
{
	const char *start;
	size_t length;

	/* Each word. */
	result->count = 0;
	start = list;
	while (*start != '\0' && result->count < RESOLVER6_SERVERS) {
		length = strcspn(start, " ");
		if (length != 0U)
			(void)snprintf(result->text[result->count++], RESOLVER6_SERVER_TEXT, "%.*s", (int)length, start);
		start += length;
		if (*start == ' ')
			start++;
	}
}

/* Reconciles two net.conf texts into program; 0, or -1. */
static int
reconcile(
	const char *before,
	const char *after)
{
	static struct netconf previous;
	static struct netconf target;
	char error[256];
	int status;

	/* Both read. */
	program[0] = '\0';
	status = parse(before, &previous);
	if (status != 0)
		return -1;
	status = parse(after, &target);
	if (status != 0)
		return -1;

	/* The program. */
	status = netconf_reconcile(&previous, &target, record, NULL, error, sizeof(error));
	if (status != 0)
		printf("  error: %s\n", error);
	return status;
}

/* Reads a net.conf text and validates it; 0, or -1. */
static int
parse(
	const char *text,
	struct netconf *configuration)
{
	char error[256];
	FILE *stream;
	int status;

	/* The text as a stream. */
	stream = fmemopen((void *)text, strlen(text), "r");
	if (stream == NULL)
		return -1;
	status = netconf_parse(stream, configuration, error, sizeof(error));
	fclose(stream);
	if (status == 0)
		status = netconf_validate(configuration, error, sizeof(error));
	if (status != 0)
		printf("  net.conf: %s\n", error);
	return status;
}

/* Records one operation of the reconcile's program. */
static int
record(
	const char *operation,
	const char *operands,
	void *context)
{
	size_t used;

	/* After the ones before it, with its operands when it has any. */
	(void)context;
	used = strlen(program);
	if (operands == NULL)
		(void)snprintf(program + used, sizeof(program) - used, "%s\n", operation);
	else
		(void)snprintf(program + used, sizeof(program) - used, "%s %s\n", operation, operands);
	return 0;
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
