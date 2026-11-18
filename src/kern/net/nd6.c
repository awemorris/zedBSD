/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Neighbor discovery (ws130-p002; RFC 4861, RFC 4862's detection).
 *
 * The neighbor cache maps an interface and an address to a link address,
 * with the states of in6-neighbor.c; a packet to a neighbor not yet
 * resolved is held (one a neighbor, the newest) until it answers.
 * Solicitations for the host's addresses are answered, advertisements
 * update the cache, a duplicate of an address under detection is told to
 * the layer, a Router Advertisement's link values are applied (its
 * prefixes, routes and DNS servers are networkd's), and a Redirect
 * becomes a host route.  The kernel is a host: it never answers a router
 * solicitation and never forwards.
 */

#include "ipv6.h"

#include "kern/net/ethernet.h"
#include "kern/net/net-device.h"
#include "kern/net/packet-buf.h"
#include "kern/net/socket.h"
#include "kern/lock.h"
#include "internal.h"
#include "wire.h"
#include <kern/kcrt.h>

#include <uapi/errno.h>
#include <uapi/route.h>

/* The work the timer gathers under the lock, at most. */
#define ND6_WORK_MAX		16U

/* The options a message may carry that are looked at. */
struct nd6_options {
	const uint8_t *source_link;
	const uint8_t *target_link;
	unsigned mtu;
};

/* One send or drop the timer found, done after the lock. */
struct nd6_work {
	unsigned actions;
	struct net_device *device;
	unsigned ifindex;
	struct in6_addr address;
	uint8_t link[6];
	struct packet_buf *held;
};

/* Guards the cache and the held packets. */
static struct spinlock nd6_lock;

/* The cache, its held packets, and the next entry to reuse when it is full. */
static struct in6_neighbor nd6_neighbors[IN6_NEIGHBORS_MAX];
static struct packet_buf *nd6_held[IN6_NEIGHBORS_MAX];
static unsigned nd6_replacement;

/* The all-nodes and all-routers groups, and the unspecified address. */
static const struct in6_addr nd6_all_nodes = { { { 0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 } } };
static const struct in6_addr nd6_all_routers = { { { 0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2 } } };
static const struct in6_addr nd6_unspecified = { { { 0 } } };

static int nd6_options_parse(const uint8_t *data, size_t length, struct nd6_options *options);
static void nd6_router_advert(struct packet_buf *packet, const struct in6_addr *source, const struct nd6_options *options);
static void nd6_neighbor_solicit(struct packet_buf *packet, const struct in6_addr *source, const struct in6_addr *target, const struct nd6_options *options);
static void nd6_neighbor_advert(struct packet_buf *packet, const struct in6_addr *destination, const struct in6_addr *target, unsigned flags, const struct nd6_options *options);
static void nd6_redirect(struct packet_buf *packet, const struct in6_addr *source, const struct nd6_options *options);
static void nd6_learn(struct net_device *device, const struct in6_addr *address, const uint8_t link[6], int router);
static int nd6_send_advertisement(struct net_device *device, const struct in6_addr *target, const struct in6_addr *destination, unsigned flags);
static struct in6_neighbor *nd6_find_locked(const struct net_device *device, const struct in6_addr *address);
static struct in6_neighbor *nd6_make_locked(struct net_device *device, const struct in6_addr *address, struct packet_buf **evicted);
static void nd6_flush(struct net_device *device, const uint8_t link[6], struct packet_buf *held);

/* Empties the cache. */
int
nd6_init(
	void)
{
	/* The lock and the empty cache. */
	spin_init(&nd6_lock, LOCK_RANK_NETWORK, "IPv6 neighbors");
	kern_memset(nd6_neighbors, 0, sizeof(nd6_neighbors));
	kern_memset(nd6_held, 0, sizeof(nd6_held));
	nd6_replacement = 0U;

	/* Succeeded. */
	return 0;
}

/*
 * Sends an IPv6 packet (its header built) to a neighbor on a device: at
 * once when its link address is known, otherwise held while the
 * neighbor is solicited.  The packet is consumed.  Returns 0 or an errno
 * value.
 */
int
nd6_output(
	struct net_device *device,
	const struct in6_addr *next_hop,
	struct packet_buf *packet)
{
	struct in6_neighbor *neighbor;
	struct packet_buf *evicted;
	struct packet_buf *replaced;
	struct ipv6_link link;
	unsigned long irq;
	unsigned actions;
	uint8_t address[6];
	int known;
	int error;

	/*
	 * A neighbor whose link address is known, its state moved on for the
	 * packet (a known entry never needs the retransmission timer): the
	 * frame goes at once.
	 */
	known = 0;
	irq = spin_lock_irqsave(&nd6_lock);

	neighbor = nd6_find_locked(device, next_hop);
	if (neighbor != NULL && neighbor->have_link) {
		(void)in6_neighbor_resolve(neighbor, ipv6_now_ms(), 0U);
		kern_memcpy(address, neighbor->link, sizeof(address));
		known = 1;
	}

	spin_unlock_irqrestore(&nd6_lock, irq);

	/* Known: sent. */
	if (known) {
		error = ethernet_output(device, address, ETHERNET_TYPE_IPV6, packet);
		return error;
	}

	/* The device's retransmission timer, for a neighbor to solicit. */
	error = ipv6_link_get(device, &link);
	if (error != 0) {
		packet_buf_free(packet);
		return error;
	}

	/* The neighbor's entry, made if there is none, and its state moved on for the packet. */
	evicted = NULL;
	replaced = NULL;
	known = 0;
	actions = 0U;
	irq = spin_lock_irqsave(&nd6_lock);

	neighbor = nd6_find_locked(device, next_hop);
	if (neighbor == NULL)
		neighbor = nd6_make_locked(device, next_hop, &evicted);
	actions = in6_neighbor_resolve(neighbor, ipv6_now_ms(), link.retrans_ms);
	if (neighbor->have_link) {
		kern_memcpy(address, neighbor->link, sizeof(address));
		known = 1;
	} else {
		replaced = nd6_held[neighbor - nd6_neighbors];
		nd6_held[neighbor - nd6_neighbors] = packet;
	}

	spin_unlock_irqrestore(&nd6_lock, irq);

	/* What the lock left: an evicted entry's packet, and an older held one. */
	if (evicted != NULL)
		packet_buf_free(evicted);
	if (replaced != NULL)
		packet_buf_free(replaced);

	/* A known neighbor: the frame goes now. */
	if (known) {
		error = ethernet_output(device, address, ETHERNET_TYPE_IPV6, packet);
		return error;
	}

	/* A new one: solicited. */
	if ((actions & ND6_ACTION_SEND_MULTICAST) != 0U)
		(void)nd6_send_solicitation(device, next_hop, NULL, NULL, 0);

	/* Succeeded: held until the neighbor answers. */
	return 0;
}

/*
 * Receives a neighbor discovery message (packet->data is the ICMPv6
 * message, its checksum checked); the packet is always consumed.  Only a
 * message with the hop limit 255 and code 0 is taken (RFC 4861 section
 * 6.1 and 7.1): no router beyond the link can have sent it.
 */
void
nd6_input(
	struct packet_buf *packet,
	const struct in6_addr *source,
	const struct in6_addr *destination,
	unsigned hop_limit)
{
	const struct icmp6_wire *message;
	const struct nd6_neighbor_wire *neighbor;
	struct nd6_options options;
	struct in6_addr target;
	size_t fixed;
	int error;

	/* From the link, with code 0. */
	message = (const struct icmp6_wire *)packet->data;
	if (hop_limit != IPV6_HOP_LIMIT_ND || message->code != 0U) {
		packet_buf_free(packet);
		return;
	}

	/* Each type's fixed part. */
	fixed = sizeof(struct nd6_rs_wire);
	if (message->type == ICMP6_ROUTER_ADVERTISEMENT)
		fixed = sizeof(struct nd6_ra_wire);
	if (message->type == ICMP6_NEIGHBOR_SOLICITATION || message->type == ICMP6_NEIGHBOR_ADVERTISEMENT)
		fixed = sizeof(struct nd6_neighbor_wire);
	if (message->type == ICMP6_REDIRECT)
		fixed = sizeof(struct nd6_redirect_wire);
	if (packet->length < fixed) {
		packet_buf_free(packet);
		return;
	}

	/* The options after it. */
	error = nd6_options_parse(packet->data + fixed, packet->length - fixed, &options);
	if (error != 0) {
		packet_buf_free(packet);
		return;
	}

	/* Each type. */
	switch (message->type) {
	case ICMP6_ROUTER_ADVERTISEMENT:
		nd6_router_advert(packet, source, &options);
		return;
	case ICMP6_NEIGHBOR_SOLICITATION:
		neighbor = (const struct nd6_neighbor_wire *)packet->data;
		kern_memcpy(target.s6_addr, neighbor->target, 16U);
		nd6_neighbor_solicit(packet, source, &target, &options);
		return;
	case ICMP6_NEIGHBOR_ADVERTISEMENT:
		neighbor = (const struct nd6_neighbor_wire *)packet->data;
		kern_memcpy(target.s6_addr, neighbor->target, 16U);
		nd6_neighbor_advert(packet, destination, &target, neighbor->header.data[0], &options);
		return;
	case ICMP6_REDIRECT:
		nd6_redirect(packet, source, &options);
		return;
	default:
		break;
	}

	/* A router solicitation: a host has nothing to say. */
	packet_buf_free(packet);
}

/*
 * Sends a Neighbor Solicitation for target: to destination (NULL: the
 * target's solicited-node group), to the link address link when given (a
 * probe of a known neighbor), and from :: without our link address for
 * duplicate address detection.  Returns 0 or an errno value.
 */
int
nd6_send_solicitation(
	struct net_device *device,
	const struct in6_addr *target,
	const struct in6_addr *destination,
	const uint8_t *link,
	int detection)
{
	struct nd6_neighbor_wire *message;
	struct nd6_option_wire *option;
	struct packet_buf *packet;
	struct in6_addr group;
	const struct in6_addr *source;
	size_t length;
	int error;

	/* To the target itself for a probe, otherwise its solicited-node group. */
	if (destination == NULL && link != NULL)
		destination = target;
	if (destination == NULL) {
		in6_solicited_node(&group, target);
		destination = &group;
	}

	/* The message, with our link address unless it is a detection. */
	length = sizeof(*message);
	if (!detection)
		length += 8U;
	packet = packet_buf_alloc(PACKET_BUF_DEFAULT_HEADROOM);
	message = NULL;
	if (packet != NULL)
		message = packet_buf_append(packet, length);
	if (message == NULL) {
		if (packet != NULL)
			packet_buf_free(packet);
		return ENOBUFS;
	}

	/* The solicitation's fields. */
	kern_memset(message, 0, length);
	message->header.type = ICMP6_NEIGHBOR_SOLICITATION;
	kern_memcpy(message->target, target->s6_addr, 16U);
	if (!detection) {
		option = (struct nd6_option_wire *)(message + 1);
		option->type = ND6_OPTION_SOURCE_LINK;
		option->length = 1U;
		kern_memcpy((uint8_t *)option + 2, device->hwaddr, 6U);
	}

	/* From :: for a detection, otherwise from the address IPv6 chooses. */
	source = NULL;
	if (detection)
		source = &nd6_unspecified;
	error = icmp6_send(device, source, destination, IPV6_HOP_LIMIT_ND, packet);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Sends a Router Solicitation to the all-routers group, from the
 * device's link-local address with our link address, or from :: without
 * it while the device has no usable address.
 */
int
nd6_send_router_solicitation(
	struct net_device *device)
{
	struct nd6_rs_wire *message;
	struct nd6_option_wire *option;
	struct packet_buf *packet;
	struct in6_addr source;
	size_t length;
	int unspecified;
	int error;

	/* The source: an address of the device's, or ::. */
	error = ipv6_source_select(device, &nd6_all_routers, &source);
	unspecified = error != 0;
	if (unspecified)
		source = nd6_unspecified;

	/* The message, with our link address when it has a source. */
	length = sizeof(*message);
	if (!unspecified)
		length += 8U;
	packet = packet_buf_alloc(PACKET_BUF_DEFAULT_HEADROOM);
	message = NULL;
	if (packet != NULL)
		message = packet_buf_append(packet, length);
	if (message == NULL) {
		if (packet != NULL)
			packet_buf_free(packet);
		return ENOBUFS;
	}

	/* The solicitation's fields. */
	kern_memset(message, 0, length);
	message->header.type = ICMP6_ROUTER_SOLICITATION;
	if (!unspecified) {
		option = (struct nd6_option_wire *)(message + 1);
		option->type = ND6_OPTION_SOURCE_LINK;
		option->length = 1U;
		kern_memcpy((uint8_t *)option + 2, device->hwaddr, 6U);
	}

	/* Sent. */
	error = icmp6_send(device, &source, &nd6_all_routers, IPV6_HOP_LIMIT_ND, packet);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Runs the neighbors' timers: solicitations again, entries gone stale or
 * probed, neighbors given up (their held packets dropped).
 */
void
nd6_timer_run(
	uint64_t now_ms)
{
	struct nd6_work work[ND6_WORK_MAX];
	struct in6_neighbor *neighbor;
	unsigned long irq;
	unsigned actions;
	unsigned count;
	unsigned index;
	int referenced;

	/* Each entry whose time came, while there is room for its work. */
	count = 0U;
	irq = spin_lock_irqsave(&nd6_lock);

	for (index = 0U; index < IN6_NEIGHBORS_MAX && count < ND6_WORK_MAX; index++) {
		neighbor = &nd6_neighbors[index];
		if (neighbor->state == ND6_NONE || neighbor->deadline_ms == 0U || now_ms < neighbor->deadline_ms)
			continue;
		work[count].device = neighbor->device;
		work[count].ifindex = neighbor->ifindex;
		work[count].address = neighbor->address;
		kern_memcpy(work[count].link, neighbor->link, 6U);
		actions = in6_neighbor_timer(neighbor, now_ms, IN6_RETRANS_MS);
		work[count].actions = actions;
		work[count].held = NULL;
		if ((actions & ND6_ACTION_DROP) != 0U || (actions & ND6_ACTION_FREE) != 0U) {
			work[count].held = nd6_held[index];
			nd6_held[index] = NULL;
		}

		/* One piece of work. */
		count++;
	}

	spin_unlock_irqrestore(&nd6_lock, irq);

	/* The drops, the routers lost, and the sends, each device held while it sends. */
	for (index = 0U; index < count; index++) {
		if (work[index].held != NULL)
			packet_buf_free(work[index].held);
		if ((work[index].actions & ND6_ACTION_UNREACHABLE) != 0U)
			route_socket_notify_neighbor(work[index].ifindex, &work[index].address, RTM_NEIGHBOR_UNREACHABLE, 1);
		referenced = net_device_ref_live(work[index].device);
		if (!referenced)
			continue;
		if ((work[index].actions & ND6_ACTION_SEND_MULTICAST) != 0U)
			(void)nd6_send_solicitation(work[index].device, &work[index].address, NULL, NULL, 0);
		if ((work[index].actions & ND6_ACTION_SEND_UNICAST) != 0U)
			(void)nd6_send_solicitation(work[index].device, &work[index].address, NULL, work[index].link, 0);
		net_device_release(work[index].device);
	}
}

/* Gives when a neighbor's timer is next due, in milliseconds (0 for none). */
uint64_t
nd6_timer_next_deadline(
	void)
{
	unsigned long irq;
	unsigned index;
	uint64_t deadline;

	/* The earliest of the entries'. */
	deadline = 0U;
	irq = spin_lock_irqsave(&nd6_lock);

	for (index = 0U; index < IN6_NEIGHBORS_MAX; index++) {
		if (nd6_neighbors[index].state == ND6_NONE || nd6_neighbors[index].deadline_ms == 0U)
			continue;
		if (deadline == 0U || nd6_neighbors[index].deadline_ms < deadline)
			deadline = nd6_neighbors[index].deadline_ms;
	}

	spin_unlock_irqrestore(&nd6_lock, irq);

	/* Succeeded: the earliest, or none. */
	return deadline;
}

/* Forgets the neighbors of a device that is going away, dropping their held packets. */
void
nd6_purge_device(
	struct net_device *device)
{
	struct packet_buf *held[IN6_NEIGHBORS_MAX];
	unsigned long irq;
	unsigned count;
	unsigned index;

	/* The device's entries. */
	count = 0U;
	irq = spin_lock_irqsave(&nd6_lock);

	for (index = 0U; index < IN6_NEIGHBORS_MAX; index++) {
		if (nd6_neighbors[index].state == ND6_NONE || nd6_neighbors[index].device != device)
			continue;
		if (nd6_held[index] != NULL) {
			held[count] = nd6_held[index];
			count++;
			nd6_held[index] = NULL;
		}

		/* The entry free. */
		kern_memset(&nd6_neighbors[index], 0, sizeof(nd6_neighbors[index]));
	}

	spin_unlock_irqrestore(&nd6_lock, irq);

	/* Their packets. */
	for (index = 0U; index < count; index++)
		packet_buf_free(held[index]);
}

/*
 * Reads the options of a message (RFC 4861 section 4.6): a zero length
 * makes the message invalid; the source and target link addresses and
 * the MTU are kept, the others passed over.  Returns 0 or EINVAL.
 */
static int
nd6_options_parse(
	const uint8_t *data,
	size_t length,
	struct nd6_options *options)
{
	const struct nd6_option_mtu_wire *mtu;
	size_t position;
	size_t size;

	/* Nothing found yet. */
	options->source_link = NULL;
	options->target_link = NULL;
	options->mtu = 0U;

	/* Each option. */
	position = 0U;
	while (position + 2U <= length) {
		size = (size_t)data[position + 1U] * 8U;
		if (size == 0U || position + size > length)
			return EINVAL;

		/* The ones looked at. */
		if (data[position] == ND6_OPTION_SOURCE_LINK && size >= 8U)
			options->source_link = data + position + 2U;
		if (data[position] == ND6_OPTION_TARGET_LINK && size >= 8U)
			options->target_link = data + position + 2U;
		if (data[position] == ND6_OPTION_MTU && size >= 8U) {
			mtu = (const struct nd6_option_mtu_wire *)(data + position);
			options->mtu = wire_get32(mtu->mtu);
		}

		/* The next one. */
		position += size;
	}

	/* Succeeded: the options read. */
	return 0;
}

/*
 * Takes a Router Advertisement (RFC 4861 section 6.1.2 and 6.3.4): from
 * a link-local address; the link values it gives are applied, the router
 * is a neighbor, and the solicitations stop.  The prefixes, routes and
 * DNS servers it carries are networkd's (the routing socket's
 * RTM_ROUTERADV, once the user interface is there).
 */
static void
nd6_router_advert(
	struct packet_buf *packet,
	const struct in6_addr *source,
	const struct nd6_options *options)
{
	const struct nd6_ra_wire *message;
	struct net_device *device;
	struct ipv6_link values;
	int linklocal;

	/* Only from a router on the link. */
	linklocal = in6_is_linklocal(source);
	if (!linklocal) {
		packet_buf_free(packet);
		return;
	}

	/* The link values (a zero leaves the value as it is). */
	message = (const struct nd6_ra_wire *)packet->data;
	values.hop_limit = message->header.data[0];
	values.reachable_ms = wire_get32(message->reachable_time);
	values.retrans_ms = wire_get32(message->retrans_timer);
	values.mtu = options->mtu;
	device = packet->device;
	ipv6_link_update(device, &values);

	/* The router as a neighbor, and no more solicitations. */
	if (options->source_link != NULL)
		nd6_learn(device, source, options->source_link, 1);
	ipv6_router_advertised(device);

	/* The message for networkd: its prefixes, routes and DNS servers. */
	route_socket_notify_routeradv(device->ifindex, source, packet->data, packet->length);
	packet_buf_free(packet);
}

/*
 * Takes a Neighbor Solicitation (RFC 4861 section 7.2.3, RFC 4862
 * section 5.4.3): a detection from :: for a tentative address of ours is
 * a duplicate; one for an address of ours is answered (to the sender, or
 * to all nodes when it came from ::), and the sender's link address
 * learned.
 */
static void
nd6_neighbor_solicit(
	struct packet_buf *packet,
	const struct in6_addr *source,
	const struct in6_addr *target,
	const struct nd6_options *options)
{
	struct net_device *device;
	unsigned flags;
	int unspecified;
	int multicast;
	int error;

	/* A unicast target that is ours. */
	device = packet->device;
	multicast = in6_is_multicast(target);
	error = ipv6_address_state(device, target, &flags);
	unspecified = in6_is_unspecified(source);
	if (multicast || error != 0 || (unspecified && options->source_link != NULL)) {
		packet_buf_free(packet);
		return;
	}

	/* Another node detecting the address we are detecting: it is a duplicate. */
	if ((flags & IN6_ADDRESS_TENTATIVE) != 0U) {
		if (unspecified)
			ipv6_duplicate(device, target);
		packet_buf_free(packet);
		return;
	}

	/* A duplicated address is not answered for. */
	if ((flags & IN6_ADDRESS_DUPLICATED) != 0U) {
		packet_buf_free(packet);
		return;
	}

	/* The sender learned, and the answer. */
	net_device_ref(device);
	if (!unspecified && options->source_link != NULL)
		nd6_learn(device, source, options->source_link, 0);
	packet_buf_free(packet);
	if (unspecified) {
		(void)nd6_send_advertisement(device, target, &nd6_all_nodes, ND6_ADVERT_OVERRIDE);
	} else {
		(void)nd6_send_advertisement(device, target, source, ND6_ADVERT_SOLICITED | ND6_ADVERT_OVERRIDE);
	}

	/* The device let go. */
	net_device_release(device);
}

/*
 * Takes a Neighbor Advertisement (RFC 4861 section 7.2.5): for a
 * tentative address of ours it is a duplicate; for a neighbor in the
 * cache it updates the entry (and a held packet may go).
 */
static void
nd6_neighbor_advert(
	struct packet_buf *packet,
	const struct in6_addr *destination,
	const struct in6_addr *target,
	unsigned flags,
	const struct nd6_options *options)
{
	struct in6_neighbor *neighbor;
	struct packet_buf *held;
	struct net_device *device;
	struct ipv6_link link;
	unsigned long irq;
	unsigned address_flags;
	unsigned actions;
	uint8_t address[6];
	int multicast;
	int solicited;
	int error;

	/* A solicited answer is never to a group; the target is unicast. */
	solicited = (flags & ND6_ADVERT_SOLICITED) != 0U;
	multicast = in6_is_multicast(destination);
	if (multicast && solicited) {
		packet_buf_free(packet);
		return;
	}

	/* The target unicast. */
	multicast = in6_is_multicast(target);
	if (multicast) {
		packet_buf_free(packet);
		return;
	}

	/* For an address of ours: a duplicate while it is tentative, otherwise nothing to learn. */
	device = packet->device;
	error = ipv6_address_state(device, target, &address_flags);
	if (error == 0) {
		if ((address_flags & IN6_ADDRESS_TENTATIVE) != 0U)
			ipv6_duplicate(device, target);
		packet_buf_free(packet);
		return;
	}

	/* The entry moved on (an advertisement makes no new entry). */
	error = ipv6_link_get(device, &link);
	if (error != 0) {
		packet_buf_free(packet);
		return;
	}

	/* The advertisement taken by the entry. */
	actions = 0U;
	held = NULL;
	irq = spin_lock_irqsave(&nd6_lock);

	neighbor = nd6_find_locked(device, target);
	if (neighbor != NULL) {
		actions = in6_neighbor_advert(neighbor, options->target_link, solicited, (flags & ND6_ADVERT_OVERRIDE) != 0U,
		    (flags & ND6_ADVERT_ROUTER) != 0U, ipv6_now_ms(), link.reachable_ms);
		kern_memcpy(address, neighbor->link, sizeof(address));
		if ((actions & ND6_ACTION_FLUSH) != 0U) {
			held = nd6_held[neighbor - nd6_neighbors];
			nd6_held[neighbor - nd6_neighbors] = NULL;
		}
	}

	spin_unlock_irqrestore(&nd6_lock, irq);

	/* A router that said it no longer is one is told; the held packet goes to the address now known. */
	if ((actions & ND6_ACTION_UNREACHABLE) != 0U)
		route_socket_notify_neighbor(device->ifindex, target, RTM_NEIGHBOR_UNREACHABLE, 1);
	net_device_ref(device);
	packet_buf_free(packet);
	if (held != NULL)
		nd6_flush(device, address, held);
	net_device_release(device);
}

/*
 * Takes a Redirect (RFC 4861 section 8.1): from the link-local address of
 * the destination's current router, to a target that is a router on the
 * link or the destination itself; the destination then goes through the
 * target, whose link address may come with it.
 */
static void
nd6_redirect(
	struct packet_buf *packet,
	const struct in6_addr *source,
	const struct nd6_options *options)
{
	const struct nd6_redirect_wire *message;
	struct net_device *device;
	struct in6_addr target;
	struct in6_addr destination;
	int linklocal;
	int multicast;
	int same;
	int error;

	/* From a router on the link, about a unicast destination, to a router on the link or the destination. */
	message = (const struct nd6_redirect_wire *)packet->data;
	kern_memcpy(target.s6_addr, message->target, 16U);
	kern_memcpy(destination.s6_addr, message->destination, 16U);
	linklocal = in6_is_linklocal(source);
	multicast = in6_is_multicast(&destination);
	if (!linklocal || multicast) {
		packet_buf_free(packet);
		return;
	}

	/* A target that is a router on the link, or the destination itself. */
	linklocal = in6_is_linklocal(&target);
	same = in6_equal(&target, &destination);
	if (!linklocal && !same) {
		packet_buf_free(packet);
		return;
	}

	/* The new way, and the target's link address. */
	device = packet->device;
	error = ipv6_route_redirect(device, &destination, &target, source);
	if (error == 0 && options->target_link != NULL)
		nd6_learn(device, &target, options->target_link, !same);
	packet_buf_free(packet);
}

/*
 * Learns a neighbor's link address from a message it sent (made stale);
 * a packet held for it goes.
 */
static void
nd6_learn(
	struct net_device *device,
	const struct in6_addr *address,
	const uint8_t link[6],
	int router)
{
	struct in6_neighbor *neighbor;
	struct packet_buf *evicted;
	struct packet_buf *held;
	unsigned long irq;
	unsigned actions;

	/* The entry, made if there is none, with the address. */
	evicted = NULL;
	held = NULL;
	irq = spin_lock_irqsave(&nd6_lock);

	neighbor = nd6_find_locked(device, address);
	if (neighbor == NULL)
		neighbor = nd6_make_locked(device, address, &evicted);
	actions = in6_neighbor_link(neighbor, link, ipv6_now_ms());
	if (router)
		neighbor->router = 1;
	if ((actions & ND6_ACTION_FLUSH) != 0U) {
		held = nd6_held[neighbor - nd6_neighbors];
		nd6_held[neighbor - nd6_neighbors] = NULL;
	}

	spin_unlock_irqrestore(&nd6_lock, irq);

	/* What the lock left. */
	if (evicted != NULL)
		packet_buf_free(evicted);
	if (held != NULL)
		nd6_flush(device, link, held);
}

/*
 * Sends a Neighbor Advertisement for target (an address of ours, its
 * source) to destination, with flags and our link address.
 */
static int
nd6_send_advertisement(
	struct net_device *device,
	const struct in6_addr *target,
	const struct in6_addr *destination,
	unsigned flags)
{
	struct nd6_neighbor_wire *message;
	struct nd6_option_wire *option;
	struct packet_buf *packet;
	size_t length;
	int error;

	/* The message and the target link address option. */
	length = sizeof(*message) + 8U;
	packet = packet_buf_alloc(PACKET_BUF_DEFAULT_HEADROOM);
	message = NULL;
	if (packet != NULL)
		message = packet_buf_append(packet, length);
	if (message == NULL) {
		if (packet != NULL)
			packet_buf_free(packet);
		return ENOBUFS;
	}

	/* The advertisement's fields. */
	kern_memset(message, 0, length);
	message->header.type = ICMP6_NEIGHBOR_ADVERTISEMENT;
	message->header.data[0] = (uint8_t)flags;
	kern_memcpy(message->target, target->s6_addr, 16U);
	option = (struct nd6_option_wire *)(message + 1);
	option->type = ND6_OPTION_TARGET_LINK;
	option->length = 1U;
	kern_memcpy((uint8_t *)option + 2, device->hwaddr, 6U);

	/* From the target address itself. */
	error = icmp6_send(device, target, destination, IPV6_HOP_LIMIT_ND, packet);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Finds a neighbor's entry; the caller holds the lock. */
static struct in6_neighbor *
nd6_find_locked(
	const struct net_device *device,
	const struct in6_addr *address)
{
	unsigned index;
	int same;

	/* Each entry in use on the device. */
	for (index = 0U; index < IN6_NEIGHBORS_MAX; index++) {
		if (nd6_neighbors[index].state == ND6_NONE || nd6_neighbors[index].device != device)
			continue;
		same = in6_equal(&nd6_neighbors[index].address, address);
		if (same)
			return &nd6_neighbors[index];
	}

	/* None. */
	return NULL;
}

/*
 * Makes an entry for a neighbor (the caller holds the lock): a free one,
 * or the next in turn when the cache is full (its held packet given back
 * in *evicted for the caller to drop).  The entry is in state NONE with
 * its device and address set.
 */
static struct in6_neighbor *
nd6_make_locked(
	struct net_device *device,
	const struct in6_addr *address,
	struct packet_buf **evicted)
{
	struct in6_neighbor *neighbor;
	unsigned index;

	/* A free entry. */
	for (index = 0U; index < IN6_NEIGHBORS_MAX; index++) {
		if (nd6_neighbors[index].state == ND6_NONE)
			break;
	}

	/* Otherwise the next in turn, its held packet given back. */
	if (index == IN6_NEIGHBORS_MAX) {
		index = nd6_replacement % IN6_NEIGHBORS_MAX;
		nd6_replacement++;
		*evicted = nd6_held[index];
		nd6_held[index] = NULL;
	}

	/* The entry, new. */
	neighbor = &nd6_neighbors[index];
	kern_memset(neighbor, 0, sizeof(*neighbor));
	neighbor->device = device;
	neighbor->ifindex = device->ifindex;
	neighbor->address = *address;
	neighbor->state = ND6_NONE;
	return neighbor;
}

/* Sends a packet that was held for a neighbor to the link address now known. */
static void
nd6_flush(
	struct net_device *device,
	const uint8_t link[6],
	struct packet_buf *held)
{
	/* The frame. */
	(void)ethernet_output(device, link, ETHERNET_TYPE_IPV6, held);
}
