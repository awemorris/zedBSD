/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The DHCPv6 messages of `dhcpc -6` (ws130-p007, RFC 8415): a client's
 * Solicit, Request, Renew, Rebind, Release, Decline (ws177-p046) and
 * Information-Request built, and a server's
 * Advertise and Reply read (its identifier, preference, status, the
 * address of an IA_NA with its times, the DNS servers and search list of
 * RFC 3646, and the information refresh time of RFC 8415 section 21.23).
 * The client's identifier is a DUID-UUID (RFC 6355, H7: no MAC address
 * shown).  Pure: the host tests build this file alone.
 */

#ifndef NET_DHCP6_H
#define NET_DHCP6_H

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>

/* The ports: a client listens on 546, servers and relays on 547. */
#define DHCP6_CLIENT_PORT	546U
#define DHCP6_SERVER_PORT	547U

/* The message types a client sends and reads. */
#define DHCP6_SOLICIT		1U
#define DHCP6_ADVERTISE		2U
#define DHCP6_REQUEST		3U
#define DHCP6_RENEW		5U
#define DHCP6_REBIND		6U
#define DHCP6_REPLY		7U
#define DHCP6_RELEASE		8U
#define DHCP6_DECLINE		9U
#define DHCP6_INFORMATION	11U

/* The status codes a client acts on (RFC 8415 section 21.13). */
#define DHCP6_STATUS_SUCCESS		0U
#define DHCP6_STATUS_NO_ADDRS		2U
#define DHCP6_STATUS_NO_BINDING		3U
#define DHCP6_STATUS_USE_MULTICAST	5U

/* "No status option": success, as the RFC reads a missing one. */
#define DHCP6_STATUS_NONE	0xffffffffU

/* The longest DUID (RFC 8415 section 11.1: 128 bytes after its type), a DUID-UUID's length, and its type. */
#define DHCP6_DUID_MAX		130U
#define DHCP6_DUID_UUID_LENGTH	18U
#define DHCP6_DUID_UUID		4U

/* The most DNS servers kept, and the longest search list (the names separated by spaces). */
#define DHCP6_DNS_MAX		3U
#define DHCP6_SEARCH_MAX	256U

/* The information refresh time a server that gives none means, and the least a client takes (RFC 8415 section 21.23). */
#define DHCP6_REFRESH_DEFAULT	86400U
#define DHCP6_REFRESH_MINIMUM	600U

/* An infinite lifetime or time. */
#define DHCP6_INFINITE		0xffffffffU

/* A DHCP Unique Identifier: its bytes and its length. */
struct dhcp6_duid {
	uint8_t bytes[DHCP6_DUID_MAX];
	size_t length;
};

/*
 * A message a client sends: its type and transaction, its identifier, the
 * server's for a Request, a Renew, a Release or a Decline, an IA_NA (with the address asked for
 * when there is one), and the elapsed time in hundredths of a second.
 */
struct dhcp6_request {
	unsigned type;
	uint32_t xid;
	const struct dhcp6_duid *client;
	const struct dhcp6_duid *server;
	int with_ia;
	uint32_t iaid;
	int with_address;
	struct in6_addr address;
	uint16_t elapsed;
};

/*
 * What a server's message says: its type, its identifier and preference,
 * its status (DHCP6_STATUS_NONE when it has none), the IA_NA's times and
 * status, the address with its lifetimes, the DNS servers and search list,
 * and the information refresh time (0 when not given).
 */
struct dhcp6_reply {
	unsigned type;
	struct dhcp6_duid server;
	unsigned preference;
	uint32_t status;
	int has_ia;
	uint32_t iaid;
	uint32_t t1;
	uint32_t t2;
	uint32_t ia_status;
	int has_address;
	struct in6_addr address;
	uint32_t preferred;
	uint32_t valid;
	struct in6_addr dns[DHCP6_DNS_MAX];
	unsigned dns_count;
	char search[DHCP6_SEARCH_MAX];
	uint32_t refresh;
};

int dhcp6_build(uint8_t *buffer, size_t capacity, size_t *length, const struct dhcp6_request *request);
int dhcp6_parse(const uint8_t *message, size_t length, uint32_t xid, const struct dhcp6_duid *client,
    struct dhcp6_reply *reply);
void dhcp6_duid_uuid(const uint8_t *random, struct dhcp6_duid *duid);
uint32_t dhcp6_iaid(const char *interface);
void dhcp6_lease_times(const struct dhcp6_reply *reply, uint32_t *t1, uint32_t *t2);
unsigned dhcp6_lease_next(uint64_t elapsed, uint32_t t2, uint32_t valid);

#endif
