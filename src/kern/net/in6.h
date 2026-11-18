/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * IPv6's pure parts (ws130-p002): the address tests, the tables of an
 * interface's addresses, of the routes and of the neighbors, and the
 * choice of a source address.  Nothing here takes a lock, sends a packet
 * or reads the clock: the caller holds what it must and passes the time
 * in milliseconds, and a function that decides on a packet says so in its
 * result.  So the host tests build these files alone
 * (plan/ws130/tests/).
 */

#ifndef KERN_NET_IN6_H
#define KERN_NET_IN6_H

#include <stddef.h>
#include <stdint.h>

#include <uapi/netinet.h>

/* The scopes of RFC 4007 (a multicast address carries its own; RFC 6724 gives the unicast ones). */
#define IN6_SCOPE_INTERFACE	0x1U
#define IN6_SCOPE_LINK		0x2U
#define IN6_SCOPE_SITE		0x5U
#define IN6_SCOPE_GLOBAL	0xeU

/*
 * The flags of an interface's address.  They are the values the user
 * interface will carry (IN6_IFF_*): the kernel sets the first three, the
 * one who adds the address the others.
 */
#define IN6_ADDRESS_TENTATIVE	0x0001U
#define IN6_ADDRESS_DUPLICATED	0x0002U
#define IN6_ADDRESS_DEPRECATED	0x0004U
#define IN6_ADDRESS_TEMPORARY	0x0008U
#define IN6_ADDRESS_AUTOCONF	0x0010U
#define IN6_ADDRESS_DHCP	0x0020U
#define IN6_ADDRESS_NODAD	0x0040U
#define IN6_ADDRESS_CALLER	(IN6_ADDRESS_TEMPORARY | IN6_ADDRESS_AUTOCONF | IN6_ADDRESS_DHCP | IN6_ADDRESS_NODAD)

/* A lifetime in seconds that never runs out. */
#define IN6_LIFETIME_FOREVER	0xffffffffU

/* The most addresses of an interface, routes and neighbors. */
#define IN6_ADDRESSES_MAX	16U
#define IN6_ROUTES_MAX		64U
#define IN6_NEIGHBORS_MAX	64U

/* Duplicate address detection: the solicitations sent, and the time after the last (RFC 4862). */
#define IN6_DAD_TRANSMITS	1U
#define IN6_RETRANS_MS		1000U

/* Neighbor discovery's constants (RFC 4861 section 10). */
#define ND6_MULTICAST_SOLICIT	3U
#define ND6_UNICAST_SOLICIT	3U
#define ND6_DELAY_MS		5000U
#define ND6_REACHABLE_MS	30000U

/*
 * What an address-table function found to do: the times it fired and what
 * became of each address, for the caller to send and to tell.
 */
#define IN6_EVENT_DAD_PROBE	1U	/* send a solicitation for the tentative address */
#define IN6_EVENT_PREFERRED	2U	/* duplicate address detection passed */
#define IN6_EVENT_DUPLICATE	3U	/* another node has the address */
#define IN6_EVENT_DEPRECATED	4U	/* the preferred lifetime ran out */
#define IN6_EVENT_EXPIRED	5U	/* the valid lifetime ran out: removed */

/* The neighbor's states (RFC 4861 section 7.3.2); NONE is a free entry. */
#define ND6_NONE		0U
#define ND6_INCOMPLETE		1U
#define ND6_REACHABLE		2U
#define ND6_STALE		3U
#define ND6_DELAY		4U
#define ND6_PROBE		5U

/* What a neighbor's change asks of the caller (bits). */
#define ND6_ACTION_SEND_MULTICAST	0x01U	/* a solicitation to the solicited-node group */
#define ND6_ACTION_SEND_UNICAST		0x02U	/* a solicitation to the neighbor's link address */
#define ND6_ACTION_FLUSH		0x04U	/* send the packet held for the neighbor */
#define ND6_ACTION_DROP			0x08U	/* drop the held packet: the neighbor did not answer */
#define ND6_ACTION_UNREACHABLE		0x10U	/* a router stopped answering: tell */
#define ND6_ACTION_FREE			0x20U	/* the entry is free again */

/*
 * One address of an interface: the address and its prefix length, its
 * IN6_ADDRESS_* flags, when its lifetimes run out (0 for never), and its
 * duplicate address detection (the solicitations left and when the next
 * step is due, 0 when none).
 */
struct in6_address {
	struct in6_addr address;
	unsigned prefixlen;
	unsigned flags;
	uint64_t valid_ms;
	uint64_t preferred_ms;
	unsigned dad_left;
	uint64_t dad_ms;
	int used;
};

/* An interface's addresses. */
struct in6_address_table {
	struct in6_address entries[IN6_ADDRESSES_MAX];
};

/* What happened to an address, as a table function reports it. */
struct in6_address_event {
	unsigned kind;
	struct in6_addr address;
	unsigned prefixlen;
	unsigned flags;
};

/*
 * One route: the destination and its prefix length, the gateway (all
 * zero for none), the interface (its index, and the caller's device
 * pointer, which the table only carries), the flags, the metric and when
 * it runs out (0 for never).
 */
struct in6_route {
	struct in6_addr destination;
	unsigned prefixlen;
	struct in6_addr gateway;
	unsigned ifindex;
	void *device;
	unsigned flags;
	unsigned metric;
	uint64_t expires_ms;
	int used;
};

/* The routes. */
struct in6_route_table {
	struct in6_route entries[IN6_ROUTES_MAX];
};

/*
 * One neighbor: the interface (index and the caller's device pointer),
 * the address, its link address when known, its state, whether it is a
 * router, the solicitations sent in this state and when the next step is
 * due (0 for none).
 */
struct in6_neighbor {
	unsigned ifindex;
	void *device;
	struct in6_addr address;
	uint8_t link[6];
	int have_link;
	unsigned state;
	int router;
	unsigned probes;
	uint64_t deadline_ms;
};

/* A candidate source address: the address entry and whether it is on the interface the packet goes out of. */
struct in6_candidate {
	const struct in6_address *entry;
	int outgoing;
};

/* The address tests (in6.c). */
int in6_equal(const struct in6_addr *left, const struct in6_addr *right);
int in6_is_unspecified(const struct in6_addr *address);
int in6_is_loopback(const struct in6_addr *address);
int in6_is_multicast(const struct in6_addr *address);
int in6_is_linklocal(const struct in6_addr *address);
int in6_is_v4mapped(const struct in6_addr *address);
unsigned in6_scope(const struct in6_addr *address);
unsigned in6_common_prefix(const struct in6_addr *left, const struct in6_addr *right);
int in6_prefix_match(const struct in6_addr *address, const struct in6_addr *prefix, unsigned prefixlen);
void in6_prefix_mask(struct in6_addr *out, const struct in6_addr *address, unsigned prefixlen);
void in6_solicited_node(struct in6_addr *out, const struct in6_addr *address);
void in6_multicast_link(uint8_t link[6], const struct in6_addr *group);
unsigned in6_policy_precedence(const struct in6_addr *address);
unsigned in6_policy_label(const struct in6_addr *address);

/* An interface's addresses (in6-address.c). */
int in6_address_add(struct in6_address_table *table, const struct in6_addr *address, unsigned prefixlen, unsigned flags,
    uint32_t valid_s, uint32_t preferred_s, uint64_t now_ms, int *added);
int in6_address_remove(struct in6_address_table *table, const struct in6_addr *address);
struct in6_address *in6_address_find(struct in6_address_table *table, const struct in6_addr *address);
unsigned in6_address_tick(struct in6_address_table *table, uint64_t now_ms, struct in6_address_event *events, unsigned capacity);
int in6_address_duplicate(struct in6_address_table *table, const struct in6_addr *address, struct in6_address_event *event);
void in6_address_restart(struct in6_address_table *table, uint64_t now_ms);
uint64_t in6_address_deadline(const struct in6_address_table *table);
const struct in6_address *in6_address_select(const struct in6_candidate *candidates, unsigned count,
    const struct in6_addr *destination, int prefer_temporary);

/* The routes (in6-route.c). */
int in6_route_add(struct in6_route_table *table, const struct in6_route *route, void **replaced);
int in6_route_delete(struct in6_route_table *table, const struct in6_addr *destination, unsigned prefixlen, unsigned ifindex,
    void **removed);
const struct in6_route *in6_route_lookup(const struct in6_route_table *table, const struct in6_addr *destination,
    unsigned ifindex, uint64_t now_ms);
unsigned in6_route_expire(struct in6_route_table *table, uint64_t now_ms, void **removed, unsigned capacity);
unsigned in6_route_purge(struct in6_route_table *table, unsigned ifindex, void **removed, unsigned capacity);
uint64_t in6_route_deadline(const struct in6_route_table *table);

/* The neighbors (in6-neighbor.c). */
unsigned in6_neighbor_resolve(struct in6_neighbor *neighbor, uint64_t now_ms, unsigned retrans_ms);
unsigned in6_neighbor_link(struct in6_neighbor *neighbor, const uint8_t link[6], uint64_t now_ms);
unsigned in6_neighbor_advert(struct in6_neighbor *neighbor, const uint8_t *link, int solicited, int override, int router,
    uint64_t now_ms, unsigned reachable_ms);
unsigned in6_neighbor_confirm(struct in6_neighbor *neighbor, uint64_t now_ms, unsigned reachable_ms);
unsigned in6_neighbor_timer(struct in6_neighbor *neighbor, uint64_t now_ms, unsigned retrans_ms);

#endif
