/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * IPv6 (RFC 8200).
 *
 * Every network device has an IPv6 record, made when the timer or the
 * input first sees the device: whether IPv6 is on (it is by default),
 * whether the device had carrier at the last look, its addresses (in6.h),
 * its link values (hop limit, MTU, reachable time, retransmission timer:
 * a Router Advertisement may change them), its router solicitations and
 * its multicast listener report.  The loopback device has ::1.  The
 * kernel makes no address of its own on another device: networkd adds the
 * link-local and the SLAAC ones, the kernel detects duplicates, counts
 * their lifetimes and solicits routers once the link-local address is
 * the interface's.
 *
 * Input checks the header, keeps a packet for one of the interface's
 * addresses (or, round the loopback device, any of the host's) or for a
 * group it listens to, walks the extension headers (a fragment goes to
 * the reassembly, ipv6-frag.c) and hands the payload to ICMPv6 or a
 * registered next header.  Output takes the route, the source address
 * (RFC 6724) and the link MTU, builds the header and gives the packet to
 * neighbor discovery, to the loopback device, or to the group's Ethernet
 * address.  No packet is forwarded.
 *
 * One lock covers the records and the routes; nothing is sent, and no
 * device reference is taken or dropped, while it is held: the timer
 * gathers its work under the lock and does it after.
 */

#include "ipv6.h"

#include "kern/net/ethernet.h"
#include "kern/net/net-device.h"
#include "kern/net/packet-buf.h"
#include "kern/net/route.h"
#include "kern/net/socket.h"
#include "kern/clock.h"
#include "kern/lock.h"
#include "internal.h"
#include "wire.h"
#include <kern/kcrt.h>

#include <uapi/errno.h>
#include <uapi/route.h>
#include "kern/klog.h"

/* The next headers a transport may register. */
#define IPV6_PROTOCOL_MAX	8U

/* The work one pass of the timer gathers, at most. */
#define IPV6_WORK_MAX		32U

/* How often the timer looks at the devices' carrier when nothing else is due. */
#define IPV6_CARRIER_POLL_MS	1000U

/* The kinds of the timer's work. */
#define IPV6_WORK_DAD		1U	/* a duplicate address detection solicitation */
#define IPV6_WORK_RS		2U	/* a router solicitation */
#define IPV6_WORK_MLD		3U	/* a multicast listener report */
#define IPV6_WORK_TELL		4U	/* an address's change, told on the routing sockets */

/* The metric of a connected route (the prefix of an address of the interface). */
#define IPV6_CONNECTED_METRIC	256U

/*
 * One device's IPv6: the device (the record holds a reference), its
 * index, whether IPv6 is on, whether it had carrier at the last look,
 * its addresses, its link values, the router solicitations left and when
 * the next is due, and when a listener report is due (0 for none).
 */
struct ipv6_interface {
	struct net_device *device;
	unsigned ifindex;
	int enabled;
	int carrier;
	struct in6_address_table addresses;
	struct ipv6_link link;
	/*
	 * The MTU the last router advertisement gave (0: none), kept whole
	 * even beyond the device's MTU: link.mtu is the smaller of the two,
	 * worked out again when the device's MTU changes.
	 */
	unsigned ra_mtu;
	unsigned solicitations;
	uint64_t solicit_ms;
	uint64_t report_ms;
};

/* A next header's input function. */
struct ipv6_protocol {
	uint8_t next_header;
	ipv6_input_fn input;
};

/* One piece of the timer's work, done after the lock is let go (a change told: its event, prefix length, flags and interface). */
struct ipv6_work {
	unsigned kind;
	struct net_device *device;
	struct in6_addr address;
	unsigned event;
	unsigned prefixlen;
	unsigned flags;
	unsigned ifindex;
};

/* Guards the records and the routes. */
static struct spinlock ipv6_lock;

/* The records, one a device. */
static struct ipv6_interface ipv6_interfaces[NET_DEVICE_MAX];

/* The routes. */
static struct in6_route_table ipv6_routes;

/* The registered next headers (set at boot, read without the lock). */
static struct ipv6_protocol ipv6_protocols[IPV6_PROTOCOL_MAX];
static unsigned ipv6_protocol_count;

/* The all-nodes group, and the loopback address. */
static const struct in6_addr ipv6_all_nodes = { { { 0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 } } };
static const struct in6_addr ipv6_loopback = { { { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 } } };

static int ipv6_input(struct packet_buf *packet);
static int ipv6_accepts(struct net_device *device, const struct in6_addr *destination);
static int ipv6_extensions(struct packet_buf **walked, uint8_t *next_header, size_t *offset, size_t *field);
static int ipv6_options(struct packet_buf *packet, size_t start, size_t length, const struct in6_addr *destination);
static int ipv6_deliver(struct packet_buf *packet, uint8_t next_header, size_t field, const struct in6_addr *source, const struct in6_addr *destination);
static int ipv6_next_hop(struct net_device **device, const struct in6_addr *destination, struct in6_addr *next_hop);
static int ipv6_header_push(struct net_device *device, const struct in6_addr *source, const struct in6_addr *destination, uint8_t next_header, unsigned hop_limit, struct packet_buf *packet);
static void ipv6_header_fill(struct ipv6_wire *header, const struct in6_addr *source, const struct in6_addr *destination, uint8_t next_header, unsigned hop_limit, size_t payload);
static void ipv6_interfaces_ensure(void);
static void ipv6_interface_ensure(struct net_device *device);
static struct ipv6_interface *ipv6_interface_locked(const struct net_device *device);
static unsigned ipv6_interface_tick_locked(struct ipv6_interface *interface, uint64_t now_ms, struct ipv6_work *work, unsigned count);
static void ipv6_work_do(const struct ipv6_work *work, unsigned count);
static unsigned ipv6_groups_locked(const struct ipv6_interface *interface, struct in6_addr *groups, unsigned capacity);
static void ipv6_connected_locked(struct ipv6_interface *interface, const struct in6_addr *address, unsigned prefixlen, int add);
static void ipv6_tell(unsigned ifindex, unsigned event, const struct in6_addr *address, unsigned prefixlen, unsigned flags);

/*
 * Initializes IPv6 and registers it with Ethernet.
 */
int
ipv6_init(
	void)
{
	int error;

	/* Empty records, routes and next headers. */
	spin_init(&ipv6_lock, LOCK_RANK_NETWORK, "IPv6");
	kern_memset(ipv6_interfaces, 0, sizeof(ipv6_interfaces));
	kern_memset(&ipv6_routes, 0, sizeof(ipv6_routes));
	kern_memset(ipv6_protocols, 0, sizeof(ipv6_protocols));
	ipv6_protocol_count = 0U;

	/* ICMPv6's path MTUs, the fragments' records, and neighbor discovery's cache. */
	icmp6_init();
	ipv6_fragment_init();
	error = nd6_init();
	if (error != 0)
		return error;

	/* IPv6 frames from Ethernet. */
	error = ethernet_protocol_register(ETHERNET_TYPE_IPV6, ipv6_input);
	if (error != 0)
		return error;

	/* Succeeded: the loopback device's ::1 comes with the first timer. */
	net_worker_wakeup();
	return 0;
}

/*
 * Registers the input function of a next header (a transport).
 */
int
ipv6_protocol_register(
	uint8_t next_header,
	ipv6_input_fn input)
{
	unsigned index;

	/* A function, for a next header that is not ICMPv6's or taken. */
	if (input == NULL || next_header == IPV6_NEXT_ICMPV6)
		return EINVAL;
	for (index = 0U; index < ipv6_protocol_count; index++) {
		if (ipv6_protocols[index].next_header == next_header)
			return EEXIST;
	}

	/* Room for it. */
	if (ipv6_protocol_count >= IPV6_PROTOCOL_MAX)
		return ENOSPC;

	/* Succeeded: registered. */
	ipv6_protocols[ipv6_protocol_count].next_header = next_header;
	ipv6_protocols[ipv6_protocol_count].input = input;
	ipv6_protocol_count++;
	return 0;
}

/*
 * Sends a payload to a destination: device (may be NULL for a routed
 * unicast destination) the interface, source (NULL: chosen by RFC 6724)
 * the source address, hop_limit 0 for the interface's (1 to a group).
 * The packet is consumed.  Returns 0 (sent, or held until the next hop's
 * link address is known), or an errno value.
 */
int
ipv6_output(
	struct net_device *device,
	const struct in6_addr *source,
	const struct in6_addr *destination,
	uint8_t next_header,
	unsigned hop_limit,
	struct packet_buf *packet)
{
	struct in6_addr next_hop;
	struct in6_addr chosen;
	uint8_t group_link[6];
	int multicast;
	int local;
	int error;

	/* A packet and a destination. */
	if (packet == NULL)
		return EINVAL;
	if (destination == NULL) {
		packet_buf_free(packet);
		return EINVAL;
	}

	/* A group needs the interface it goes out of. */
	multicast = in6_is_multicast(destination);
	if (multicast && device == NULL) {
		packet_buf_free(packet);
		return EINVAL;
	}

	/*
	 * The interface: the given one for a group, the loopback device for
	 * the host's own address, the route's for another unicast destination.
	 */
	local = 0;
	if (!multicast) {
		local = in6_is_loopback(destination);
		if (!local)
			local = ipv6_address_is_local(destination);
	}

	/* The device held for the send. */
	if (multicast) {
		net_device_ref(device);
	} else if (local) {
		device = net_loopback_ref();
		if (device == NULL) {
			packet_buf_free(packet);
			return ENETUNREACH;
		}
	} else {
		error = ipv6_next_hop(&device, destination, &next_hop);
		if (error != 0) {
			packet_buf_free(packet);
			return error;
		}
	}

	/* The source: given, or chosen for the destination. */
	if (source == NULL) {
		error = ipv6_source_select(device, destination, &chosen);
		if (error != 0) {
			net_device_release(device);
			packet_buf_free(packet);
			return error;
		}

		/* The chosen one. */
		source = &chosen;
	}

	/* The header, a group's with the hop limit 1 unless one is given. */
	if (hop_limit == 0U && multicast)
		hop_limit = 1U;
	error = ipv6_header_push(device, source, destination, next_header, hop_limit, packet);
	if (error != 0) {
		net_device_release(device);
		packet_buf_free(packet);
		return error;
	}

	/* A group: its Ethernet address. */
	if (multicast) {
		in6_multicast_link(group_link, destination);
		error = ethernet_output(device, group_link, ETHERNET_TYPE_IPV6, packet);
		net_device_release(device);
		return error;
	}

	/* The host's own address: round the loopback device. */
	if (local) {
		error = ethernet_output(device, device->hwaddr, ETHERNET_TYPE_IPV6, packet);
		net_device_release(device);
		return error;
	}

	/* A neighbor: resolved (or the packet held) by neighbor discovery. */
	error = nd6_output(device, &next_hop, packet);
	net_device_release(device);
	if (error != 0)
		return error;

	/* Succeeded: sent or held. */
	return 0;
}

/*
 * Sends a unicast payload the way ipv6_route_way found: through device
 * (the caller keeps its reference) to the way's next hop, from source,
 * with its hop limit, within its MTU (no fragment is made).  The packet
 * is consumed.  Returns 0 (sent, or held until the next hop's link
 * address is known), or an errno value.
 */
int
ipv6_output_way(
	struct net_device *device,
	const struct in6_addr *source,
	const struct in6_addr *destination,
	const struct ipv6_way *way,
	uint8_t next_header,
	struct packet_buf *packet)
{
	struct ipv6_wire *header;
	size_t payload;
	int error;

	/* A packet, a way and the addresses. */
	if (packet == NULL)
		return EINVAL;
	if (device == NULL || source == NULL || destination == NULL || way == NULL) {
		packet_buf_free(packet);
		return EINVAL;
	}

	/* A payload the way does not carry is refused. */
	payload = packet->length;
	if (payload + IPV6_HEADER_LENGTH > way->mtu || payload > 0xffffU) {
		packet_buf_free(packet);
		return EMSGSIZE;
	}

	/* The header. */
	header = packet_buf_push(packet, sizeof(*header));
	if (header == NULL) {
		packet_buf_free(packet);
		return ENOBUFS;
	}

	/* Its fields. */
	ipv6_header_fill(header, source, destination, next_header, way->hop_limit, payload);
	packet->l3_offset = (uint16_t)(packet->data - packet->storage);

	/* A neighbor: resolved (or the packet held) by neighbor discovery. */
	error = nd6_output(device, &way->next_hop, packet);
	if (error != 0)
		return error;

	/* Succeeded: sent or held. */
	return 0;
}

/*
 * Finds the way ipv6_output sends a unicast packet to a destination that
 * is not one of the host's own: the interface (referenced; the caller
 * releases it), the next hop, the interface's hop limit and the path's
 * MTU.  device (may be NULL) is the interface asked for.  A sender that
 * keeps the way checks route_generation(), which every change of the
 * routes, the addresses, the link values and the path MTUs moves.
 * Returns 0, EINVAL (a group, or the host's own address), ENETUNREACH,
 * EHOSTUNREACH or ENODEV.
 */
int
ipv6_route_way(
	struct net_device *device,
	const struct in6_addr *destination,
	struct net_device **output,
	struct ipv6_way *way)
{
	struct ipv6_link link;
	struct net_device *found;
	int multicast;
	int local;
	int error;

	/* Only a unicast destination elsewhere has a way to keep. */
	if (destination == NULL || output == NULL || way == NULL)
		return EINVAL;
	multicast = in6_is_multicast(destination);
	if (multicast)
		return EINVAL;
	local = in6_is_loopback(destination);
	if (!local)
		local = ipv6_address_is_local(destination);
	if (local)
		return EINVAL;

	/* The route, on the interface asked for when there is one. */
	found = device;
	error = ipv6_next_hop(&found, destination, &way->next_hop);
	if (error != 0)
		return error;

	/* The interface's hop limit and the path's MTU. */
	error = ipv6_link_get(found, &link);
	if (error != 0) {
		net_device_release(found);
		return error;
	}
	way->hop_limit = link.hop_limit;
	way->mtu = icmp6_path_mtu(destination, link.mtu);

	/* Succeeded: the interface held and the way. */
	*output = found;
	return 0;
}

/*
 * Finds what ipv6_output would send a packet to a destination with (the
 * transports need the source for their checksums before the
 * packet is built): the source address (RFC 6724; the destination itself
 * for one of the host's), and the path MTU when mtu is not NULL.  device
 * (may be NULL, but not for a group) is the interface asked for.
 * Returns 0, EINVAL, ENETUNREACH, EHOSTUNREACH, EADDRNOTAVAIL or ENODEV.
 */
int
ipv6_route_source(
	struct net_device *device,
	const struct in6_addr *destination,
	struct in6_addr *source,
	unsigned *mtu)
{
	struct ipv6_link link;
	struct in6_addr next_hop;
	int multicast;
	int local;
	int error;

	/* A destination, and the interface of a group. */
	if (destination == NULL || source == NULL)
		return EINVAL;
	multicast = in6_is_multicast(destination);
	if (multicast && device == NULL)
		return EINVAL;

	/* The interface, as ipv6_output chooses it. */
	local = 0;
	if (!multicast) {
		local = in6_is_loopback(destination);
		if (!local)
			local = ipv6_address_is_local(destination);
	}

	/* The device held while it is asked. */
	if (multicast) {
		net_device_ref(device);
	} else if (local) {
		device = net_loopback_ref();
		if (device == NULL)
			return ENETUNREACH;
	} else {
		error = ipv6_next_hop(&device, destination, &next_hop);
		if (error != 0)
			return error;
	}

	/* The source: the host's own destination itself, or the chosen one. */
	if (local) {
		*source = *destination;
	} else {
		error = ipv6_source_select(device, destination, source);
		if (error != 0) {
			net_device_release(device);
			return error;
		}
	}

	/* The path MTU, when it is asked for. */
	if (mtu != NULL) {
		error = ipv6_link_get(device, &link);
		if (error != 0) {
			net_device_release(device);
			return error;
		}
		*mtu = icmp6_path_mtu(destination, link.mtu);
	}

	/* Succeeded: the source (and the MTU). */
	net_device_release(device);
	return 0;
}

/*
 * Chooses the source address for a destination (RFC 6724): among the
 * addresses of every interface where IPv6 is on, or of device alone for
 * a link-local or link-scope destination; device (may be NULL) is the
 * outgoing interface.  Returns 0 or EADDRNOTAVAIL.
 */
int
ipv6_source_select(
	struct net_device *device,
	const struct in6_addr *destination,
	struct in6_addr *source)
{
	struct in6_candidate candidates[NET_DEVICE_MAX * IN6_ADDRESSES_MAX];
	const struct in6_address *chosen;
	struct ipv6_interface *interface;
	unsigned long irq;
	unsigned scope;
	unsigned count;
	unsigned index;
	unsigned entry;
	int link_only;
	int found;

	/* A destination on the link takes an address of the outgoing interface only. */
	scope = in6_scope(destination);
	link_only = 0;
	if (scope <= IN6_SCOPE_LINK && device != NULL)
		link_only = 1;

	/* The candidates, and the choice. */
	irq = spin_lock_irqsave(&ipv6_lock);

	count = 0U;
	for (index = 0U; index < NET_DEVICE_MAX; index++) {
		interface = &ipv6_interfaces[index];
		if (interface->device == NULL || !interface->enabled)
			continue;
		if (link_only && interface->device != device)
			continue;
		for (entry = 0U; entry < IN6_ADDRESSES_MAX; entry++) {
			if (!interface->addresses.entries[entry].used)
				continue;
			candidates[count].entry = &interface->addresses.entries[entry];
			candidates[count].outgoing = interface->device == device;
			count++;
		}
	}

	/* The best of them. */
	chosen = in6_address_select(candidates, count, destination, 1);
	found = 0;
	if (chosen != NULL) {
		*source = chosen->address;
		found = 1;
	}

	spin_unlock_irqrestore(&ipv6_lock, irq);

	/* None that may be used. */
	if (!found)
		return EADDRNOTAVAIL;

	/* Succeeded: the source. */
	return 0;
}

/*
 * Tells whether a device (NULL: any) has an address, with its flags.
 * Returns 0 or ENOENT.
 */
int
ipv6_address_state(
	struct net_device *device,
	const struct in6_addr *address,
	unsigned *flags)
{
	struct ipv6_interface *interface;
	struct in6_address *entry;
	unsigned long irq;
	unsigned index;
	int found;

	/* The address on the device, or on any. */
	irq = spin_lock_irqsave(&ipv6_lock);

	found = 0;
	for (index = 0U; index < NET_DEVICE_MAX && !found; index++) {
		interface = &ipv6_interfaces[index];
		if (interface->device == NULL || !interface->enabled)
			continue;
		if (device != NULL && interface->device != device)
			continue;
		entry = in6_address_find(&interface->addresses, address);
		if (entry == NULL)
			continue;
		*flags = entry->flags;
		found = 1;
	}

	spin_unlock_irqrestore(&ipv6_lock, irq);

	/* Not the host's. */
	if (!found)
		return ENOENT;

	/* Succeeded: the address and its flags. */
	return 0;
}

/* Tells whether an address is one of the host's own that may be used (not tentative or duplicated). */
int
ipv6_address_is_local(
	const struct in6_addr *address)
{
	unsigned flags;
	int error;

	/* The address on some interface. */
	error = ipv6_address_state(NULL, address, &flags);
	if (error != 0)
		return 0;

	/* Not one being checked or taken by another node. */
	if ((flags & (IN6_ADDRESS_TENTATIVE | IN6_ADDRESS_DUPLICATED)) != 0U)
		return 0;
	return 1;
}

/* Gives an interface's link values.  Returns 0 or ENODEV. */
int
ipv6_link_get(
	struct net_device *device,
	struct ipv6_link *link)
{
	struct ipv6_interface *interface;
	unsigned long irq;
	int found;

	/* The record's values. */
	irq = spin_lock_irqsave(&ipv6_lock);

	found = 0;
	interface = ipv6_interface_locked(device);
	if (interface != NULL && interface->enabled) {
		*link = interface->link;
		found = 1;
	}

	spin_unlock_irqrestore(&ipv6_lock, irq);

	/* No IPv6 there. */
	if (!found)
		return ENODEV;

	/* Succeeded: the values. */
	return 0;
}

/*
 * Takes the link values a Router Advertisement gave (a zero means the
 * router left that value unspecified; an MTU is kept within the device's
 * and above the minimum).
 */
void
ipv6_link_update(
	struct net_device *device,
	const struct ipv6_link *values)
{
	struct ipv6_interface *interface;
	unsigned long irq;
	unsigned hop_limit;
	unsigned mtu;
	int changed;

	/* The record's values, each one given. */
	changed = 0;
	irq = spin_lock_irqsave(&ipv6_lock);

	interface = ipv6_interface_locked(device);
	if (interface != NULL) {
		hop_limit = interface->link.hop_limit;
		mtu = interface->link.mtu;
		if (values->hop_limit != 0U)
			interface->link.hop_limit = values->hop_limit;
		if (values->reachable_ms != 0U)
			interface->link.reachable_ms = values->reachable_ms;
		if (values->retrans_ms != 0U)
			interface->link.retrans_ms = values->retrans_ms;
		if (values->mtu >= IPV6_MINIMUM_MTU) {
			interface->ra_mtu = values->mtu;
			interface->link.mtu = values->mtu;
			if (values->mtu > device->mtu)
				interface->link.mtu = device->mtu;
		}
		if (interface->link.hop_limit != hop_limit || interface->link.mtu != mtu)
			changed = 1;
	}

	spin_unlock_irqrestore(&ipv6_lock, irq);

	/* A new hop limit or MTU: what senders kept of the way is looked up again. */
	if (changed)
		route_changed();
}

/*
 * Takes a device's new MTU: the link MTU becomes the device's, or the
 * router's when it advertised a smaller one, and the path MTUs learnt
 * are forgotten (they are learnt again from the next Packet Too Big).
 */
void
ipv6_device_mtu_changed(
	struct net_device *device)
{
	struct ipv6_interface *interface;
	unsigned long irq;
	unsigned mtu;

	/* The link MTU worked out again from the device's and the router's. */
	irq = spin_lock_irqsave(&ipv6_lock);

	interface = ipv6_interface_locked(device);
	if (interface != NULL) {
		mtu = device->mtu;
		if (interface->ra_mtu != 0U && interface->ra_mtu < mtu)
			mtu = interface->ra_mtu;
		interface->link.mtu = mtu;
	}

	spin_unlock_irqrestore(&ipv6_lock, irq);

	/* Forgets every path MTU, which the old link MTU bounded; what senders kept of the way is looked up again. */
	icmp6_path_mtu_purge();
	route_changed();
}

/* Takes the news that another node has a tentative address of the device's (neighbor discovery). */
void
ipv6_duplicate(
	struct net_device *device,
	const struct in6_addr *address)
{
	struct ipv6_interface *interface;
	struct in6_address_event event;
	unsigned long irq;
	int duplicate;

	/* The address marked duplicated. */
	irq = spin_lock_irqsave(&ipv6_lock);

	duplicate = 0;
	interface = ipv6_interface_locked(device);
	if (interface != NULL)
		duplicate = in6_address_duplicate(&interface->addresses, address, &event);

	spin_unlock_irqrestore(&ipv6_lock, irq);

	/* Said in the log and on the routing sockets (networkd makes another address); kept ways are looked up again. */
	if (duplicate) {
		route_changed();
		kern_logf("ipv6: %s: duplicate address detected\n", device->name);
		ipv6_tell(device->ifindex, event.kind, &event.address, event.prefixlen, event.flags);
	}
}

/* A router answered: no more router solicitations on the device. */
void
ipv6_router_advertised(
	struct net_device *device)
{
	struct ipv6_interface *interface;
	unsigned long irq;

	/* The solicitations stop. */
	irq = spin_lock_irqsave(&ipv6_lock);

	interface = ipv6_interface_locked(device);
	if (interface != NULL)
		interface->solicitations = 0U;

	spin_unlock_irqrestore(&ipv6_lock, irq);
}

/*
 * Adds an address to a device, or renews one it has (its lifetimes and
 * the caller's flags): a new one is detected (unless NODAD), its group
 * reported, and its prefix (shorter than 128) made a connected route.
 * Returns 0, ENODEV (no IPv6 there), EINVAL or ENOSPC.
 */
int
ipv6_address_add(
	struct net_device *device,
	const struct in6_addr *address,
	unsigned prefixlen,
	unsigned flags,
	uint32_t valid_s,
	uint32_t preferred_s)
{
	struct ipv6_interface *interface;
	unsigned long irq;
	unsigned device_flags;
	uint64_t now;
	int added;
	int error;

	/*
	 * The loopback device has no other node to take an address, and the
	 * timer runs no detection there: its addresses are used at once
	 * (RFC 4862 section 5.4), not left tentative.
	 */
	device_flags = net_device_flags_get(device);
	if ((device_flags & NET_DEVICE_LOOPBACK) != 0U)
		flags |= IN6_ADDRESS_NODAD;

	/* The address in the device's table. */
	now = ipv6_now_ms();
	added = 0;
	irq = spin_lock_irqsave(&ipv6_lock);

	interface = ipv6_interface_locked(device);
	error = ENODEV;
	if (interface != NULL && interface->enabled)
		error = in6_address_add(&interface->addresses, address, prefixlen, flags, valid_s, preferred_s, now, &added);

	/* A new one: its group reported, its prefix connected. */
	if (error == 0 && added) {
		interface->report_ms = now;
		ipv6_connected_locked(interface, address, prefixlen, 1);
	}

	spin_unlock_irqrestore(&ipv6_lock, irq);

	/* Not added. */
	if (error != 0)
		return error;

	/* Succeeded: what senders kept is looked up again, and the timer runs the detection. */
	route_changed();
	net_worker_wakeup();
	return 0;
}

/* Removes an address of a device, with its connected route.  Returns 0, ENODEV or ENOENT. */
int
ipv6_address_remove(
	struct net_device *device,
	const struct in6_addr *address)
{
	struct ipv6_interface *interface;
	struct in6_address *entry;
	struct in6_addr removed;
	unsigned long irq;
	unsigned prefixlen;
	int error;

	/* The address, and its connected route once no other address has its prefix. */
	irq = spin_lock_irqsave(&ipv6_lock);

	interface = ipv6_interface_locked(device);
	error = ENODEV;
	if (interface != NULL) {
		error = ENOENT;
		entry = in6_address_find(&interface->addresses, address);
		if (entry != NULL) {
			removed = entry->address;
			prefixlen = entry->prefixlen;
			error = in6_address_remove(&interface->addresses, address);
			ipv6_connected_locked(interface, &removed, prefixlen, 0);
		}
	}

	spin_unlock_irqrestore(&ipv6_lock, irq);

	/* Not removed. */
	if (error != 0)
		return error;

	/* Succeeded: what senders kept of the address and its route is looked up again. */
	route_changed();
	return 0;
}

/*
 * Copies a device's addresses (at most capacity) and says whether IPv6 is
 * on there.  Returns 0 or ENODEV.
 */
int
ipv6_address_list(
	struct net_device *device,
	struct in6_address *entries,
	unsigned capacity,
	unsigned *count,
	int *enabled)
{
	struct ipv6_interface *interface;
	unsigned long irq;
	unsigned index;
	int error;

	/* The record's used entries. */
	*count = 0U;
	*enabled = 0;
	irq = spin_lock_irqsave(&ipv6_lock);

	interface = ipv6_interface_locked(device);
	error = ENODEV;
	if (interface != NULL) {
		error = 0;
		*enabled = interface->enabled;
		for (index = 0U; index < IN6_ADDRESSES_MAX && *count < capacity; index++) {
			if (!interface->addresses.entries[index].used)
				continue;
			entries[*count] = interface->addresses.entries[index];
			(*count)++;
		}
	}

	spin_unlock_irqrestore(&ipv6_lock, irq);

	/* No IPv6 record. */
	if (error != 0)
		return error;

	/* Succeeded: the addresses. */
	return 0;
}

/*
 * Turns IPv6 on or off on a device; off, its addresses, routes and
 * neighbors go.  Returns 0 or ENODEV.
 */
int
ipv6_enable(
	struct net_device *device,
	int enabled)
{
	struct ipv6_interface *interface;
	void *removed[IN6_ROUTES_MAX];
	unsigned long irq;
	int found;

	/* The record's switch; off empties it. */
	found = 0;
	irq = spin_lock_irqsave(&ipv6_lock);

	interface = ipv6_interface_locked(device);
	if (interface != NULL) {
		found = 1;
		interface->enabled = enabled;
		if (!enabled) {
			kern_memset(&interface->addresses, 0, sizeof(interface->addresses));
			interface->solicitations = 0U;
			interface->report_ms = 0U;
			(void)in6_route_purge(&ipv6_routes, interface->ifindex, removed, IN6_ROUTES_MAX);
		}
	}

	spin_unlock_irqrestore(&ipv6_lock, irq);

	/* No record. */
	if (!found)
		return ENODEV;

	/* What senders kept is looked up again. */
	route_changed();

	/* Off: no neighbor stays. */
	if (!enabled)
		nd6_purge_device(device);

	/* Succeeded. */
	return 0;
}

/* Deletes a route.  Returns 0 or ENOENT. */
int
ipv6_route_delete(
	const struct in6_addr *destination,
	unsigned prefixlen,
	unsigned ifindex)
{
	void *removed;
	unsigned long irq;
	int error;

	/* The route. */
	irq = spin_lock_irqsave(&ipv6_lock);

	error = in6_route_delete(&ipv6_routes, destination, prefixlen, ifindex, &removed);

	spin_unlock_irqrestore(&ipv6_lock, irq);

	/* Not there. */
	if (error != 0)
		return error;

	/* Succeeded: what senders kept is looked up again. */
	route_changed();
	return 0;
}

/* Copies the route at an ordinal (among the used entries).  Returns 0 or ENOENT. */
int
ipv6_route_get(
	unsigned ordinal,
	struct in6_route *route)
{
	unsigned long irq;
	unsigned index;
	unsigned seen;
	int found;

	/* The ordinal-th used entry. */
	found = 0;
	seen = 0U;
	irq = spin_lock_irqsave(&ipv6_lock);

	for (index = 0U; index < IN6_ROUTES_MAX && !found; index++) {
		if (!ipv6_routes.entries[index].used)
			continue;
		if (seen == ordinal) {
			*route = ipv6_routes.entries[index];
			route->device = NULL;
			found = 1;
		}

		/* One more route passed. */
		seen++;
	}

	spin_unlock_irqrestore(&ipv6_lock, irq);

	/* Past the last. */
	if (!found)
		return ENOENT;

	/* Succeeded: the route. */
	return 0;
}

/* Writes the groups a device reports (MLD), at most capacity.  Returns how many. */
unsigned
ipv6_groups(
	struct net_device *device,
	struct in6_addr *groups,
	unsigned capacity)
{
	struct ipv6_interface *interface;
	unsigned long irq;
	unsigned count;

	/* The record's groups. */
	count = 0U;
	irq = spin_lock_irqsave(&ipv6_lock);

	interface = ipv6_interface_locked(device);
	if (interface != NULL && interface->enabled)
		count = ipv6_groups_locked(interface, groups, capacity);

	spin_unlock_irqrestore(&ipv6_lock, irq);

	/* Succeeded: the groups. */
	return count;
}

/* Adds (or renews) a route.  Returns 0, EINVAL, ENODEV or ENOSPC. */
int
ipv6_route_add(
	const struct in6_route *route)
{
	struct ipv6_interface *interface;
	struct in6_route copy;
	void *replaced;
	unsigned long irq;
	int error;

	/* The route on a device that has an IPv6 record. */
	irq = spin_lock_irqsave(&ipv6_lock);

	interface = ipv6_interface_locked(route->device);
	error = ENODEV;
	if (interface != NULL) {
		copy = *route;
		copy.ifindex = interface->ifindex;
		error = in6_route_add(&ipv6_routes, &copy, &replaced);
	}

	spin_unlock_irqrestore(&ipv6_lock, irq);

	/* Not added. */
	if (error != 0)
		return error;

	/* Succeeded: added (the routes borrow the record's device reference); kept ways are looked up again. */
	route_changed();
	return 0;
}

/*
 * Takes a Redirect (RFC 4861 section 8.3): when source is the route's
 * next hop for the destination, a host route to it goes through gateway
 * (or is on the link when gateway is the destination) for a while.
 * Returns 0 or an errno value.
 */
int
ipv6_route_redirect(
	struct net_device *device,
	const struct in6_addr *destination,
	const struct in6_addr *gateway,
	const struct in6_addr *source)
{
	const struct in6_route *current;
	struct in6_route route;
	struct ipv6_interface *interface;
	void *replaced;
	unsigned long irq;
	uint64_t now;
	int same;
	int on_link;
	int error;

	/* The route the destination takes now, through the redirect's sender. */
	now = ipv6_now_ms();
	irq = spin_lock_irqsave(&ipv6_lock);

	interface = ipv6_interface_locked(device);
	current = NULL;
	if (interface != NULL)
		current = in6_route_lookup(&ipv6_routes, destination, interface->ifindex, now);
	error = EINVAL;
	if (current != NULL) {
		same = in6_equal(&current->gateway, source);
		if (same)
			error = 0;
	}

	/* The host route, through the target or on the link. */
	if (error == 0) {
		kern_memset(&route, 0, sizeof(route));
		route.destination = *destination;
		route.prefixlen = 128U;
		on_link = in6_equal(gateway, destination);
		if (!on_link)
			route.gateway = *gateway;
		route.ifindex = interface->ifindex;
		route.device = interface->device;
		route.flags = RTF_HOST | RTF_DYNAMIC;
		route.expires_ms = now + IPV6_REDIRECT_MS;
		error = in6_route_add(&ipv6_routes, &route, &replaced);
	}

	spin_unlock_irqrestore(&ipv6_lock, irq);

	/* Not taken. */
	if (error != 0)
		return error;

	/* Succeeded: the destination goes the new way, which senders that kept the old one look up. */
	route_changed();
	return 0;
}

/*
 * Runs IPv6's timers once a pass of the network worker: new devices get
 * their records; a device whose carrier came back detects its addresses
 * again; the addresses' detection and lifetimes, the router solicitations
 * and the listener reports move on; routes that ran out go; and the
 * neighbors' timers run.
 */
void
ipv6_timer_run(
	void)
{
	struct ipv6_work work[IPV6_WORK_MAX];
	void *expired[8];
	unsigned long irq;
	unsigned expired_count;
	unsigned count;
	unsigned index;
	unsigned told;
	uint64_t now;

	/* Every device has its record. */
	ipv6_interfaces_ensure();

	/* Each record's due work, and the routes run out. */
	now = ipv6_now_ms();
	irq = spin_lock_irqsave(&ipv6_lock);

	count = 0U;
	for (index = 0U; index < NET_DEVICE_MAX; index++) {
		if (ipv6_interfaces[index].device == NULL)
			continue;
		count = ipv6_interface_tick_locked(&ipv6_interfaces[index], now, work, count);
	}

	/* The routes run out. */
	expired_count = in6_route_expire(&ipv6_routes, now, expired, 8U);

	spin_unlock_irqrestore(&ipv6_lock, irq);

	/*
	 * A route gone, or an address changed (detected, deprecated, run
	 * out, or checked again on the carrier's return: each is told):
	 * what senders kept is looked up again.
	 */
	told = 0U;
	for (index = 0U; index < count; index++) {
		if (work[index].kind == IPV6_WORK_TELL)
			told++;
	}
	if (expired_count != 0U || told != 0U)
		route_changed();

	/* The work, then the neighbors and the fragments' records. */
	ipv6_work_do(work, count);
	nd6_timer_run(now);
	ipv6_fragment_timer_run(now);
}

/*
 * Gives the tick at which IPv6's timers are next due (0 for none): the
 * earliest of the addresses', the solicitations', the reports', the
 * routes' and the neighbors' times, and the next look at the carrier
 * while a device has IPv6.
 */
uint64_t
ipv6_timer_next_deadline(
	void)
{
	struct ipv6_interface *interface;
	unsigned long irq;
	unsigned index;
	uint64_t deadline;
	uint64_t candidate;
	uint64_t now;
	int any;

	/* The earliest time of every record and of the routes. */
	now = ipv6_now_ms();
	deadline = 0U;
	any = 0;
	irq = spin_lock_irqsave(&ipv6_lock);

	for (index = 0U; index < NET_DEVICE_MAX; index++) {
		interface = &ipv6_interfaces[index];
		if (interface->device == NULL)
			continue;
		any = 1;
		candidate = in6_address_deadline(&interface->addresses);
		if (candidate != 0U && (deadline == 0U || candidate < deadline))
			deadline = candidate;
		if (interface->solicitations != 0U && (deadline == 0U || interface->solicit_ms < deadline))
			deadline = interface->solicit_ms;
		if (interface->report_ms != 0U && (deadline == 0U || interface->report_ms < deadline))
			deadline = interface->report_ms;
	}

	/* The routes'. */
	candidate = in6_route_deadline(&ipv6_routes);
	if (candidate != 0U && (deadline == 0U || candidate < deadline))
		deadline = candidate;

	spin_unlock_irqrestore(&ipv6_lock, irq);

	/* The neighbors', the fragments' records', and the carrier's look. */
	candidate = nd6_timer_next_deadline();
	if (candidate != 0U && (deadline == 0U || candidate < deadline))
		deadline = candidate;
	candidate = ipv6_fragment_deadline();
	if (candidate != 0U && (deadline == 0U || candidate < deadline))
		deadline = candidate;
	candidate = now + IPV6_CARRIER_POLL_MS;
	if (any && (deadline == 0U || candidate < deadline))
		deadline = candidate;

	/* Nothing due. */
	if (deadline == 0U)
		return 0U;

	/* Succeeded: the time in ticks. */
	return kern_ms_to_ticks(deadline);
}

/*
 * Forgets a device that is going away: its record (and the reference it
 * held), its routes and its neighbors.
 */
void
ipv6_purge_device(
	struct net_device *device)
{
	struct net_device *release;
	void *removed[IN6_ROUTES_MAX];
	unsigned long irq;
	unsigned index;

	/* The record and the routes. */
	if (device == NULL)
		return;
	release = NULL;
	irq = spin_lock_irqsave(&ipv6_lock);

	for (index = 0U; index < NET_DEVICE_MAX; index++) {
		if (ipv6_interfaces[index].device != device)
			continue;
		(void)in6_route_purge(&ipv6_routes, ipv6_interfaces[index].ifindex, removed, IN6_ROUTES_MAX);
		release = device;
		kern_memset(&ipv6_interfaces[index], 0, sizeof(ipv6_interfaces[index]));
	}

	spin_unlock_irqrestore(&ipv6_lock, irq);

	/* What senders kept is looked up again; the neighbors, the fragments, and the record's reference, go. */
	route_changed();
	nd6_purge_device(device);
	ipv6_fragment_purge_device(device);
	if (release != NULL)
		net_device_release(release);
}

/* Gives the time in milliseconds since boot. */
uint64_t
ipv6_now_ms(
	void)
{
	uint64_t ticks;
	uint64_t milliseconds;

	/* The clock's ticks, in milliseconds. */
	ticks = clock_ticks();
	milliseconds = kern_ticks_to_ms(ticks);
	return milliseconds;
}

/* Receives an IPv6 packet from Ethernet; the packet is always consumed. */
static int
ipv6_input(
	struct packet_buf *packet)
{
	const struct ipv6_wire *header;
	struct in6_addr source;
	struct in6_addr destination;
	uint8_t next_header;
	size_t payload;
	size_t offset;
	size_t field;
	void *pulled;
	int accepted;
	int multicast;
	int error;

	/* A device and a whole header of version 6. */
	if (packet == NULL || packet->device == NULL || packet->length < IPV6_HEADER_LENGTH) {
		packet_buf_free(packet);
		return EINVAL;
	}

	/* Its version and length. */
	header = (const struct ipv6_wire *)packet->data;
	payload = wire_get16(header->payload_length);
	if ((header->version_class_flow[0] >> 4) != 6U || payload > packet->length - IPV6_HEADER_LENGTH) {
		packet_buf_free(packet);
		return EINVAL;
	}

	/* The addresses; a group is never a source. */
	kern_memcpy(source.s6_addr, header->source, 16U);
	kern_memcpy(destination.s6_addr, header->destination, 16U);
	multicast = in6_is_multicast(&source);
	if (multicast) {
		packet_buf_free(packet);
		return EINVAL;
	}

	/* Only a packet for this interface. */
	accepted = ipv6_accepts(packet->device, &destination);
	if (!accepted) {
		packet_buf_free(packet);
		return 0;
	}

	/* The frame's padding cut; where the header is. */
	error = packet_buf_trim(packet, IPV6_HEADER_LENGTH + payload);
	if (error != 0) {
		packet_buf_free(packet);
		return EINVAL;
	}

	/* Where the header is. */
	packet->l3_offset = (uint16_t)(packet->data - packet->storage);
	packet->l3_length = (uint16_t)(IPV6_HEADER_LENGTH + payload);

	/* The extension headers walked to the upper layer. */
	next_header = header->next_header;
	offset = IPV6_HEADER_LENGTH;
	field = 6U;
	error = ipv6_extensions(&packet, &next_header, &offset, &field);
	if (error != 0)
		return 0;

	/* The upper layer's message alone. */
	pulled = packet_buf_pull(packet, offset);
	if (pulled == NULL) {
		packet_buf_free(packet);
		return EINVAL;
	}

	/* Where the message is. */
	packet->l4_offset = (uint16_t)(packet->data - packet->storage);

	/* Succeeded: handed on. */
	error = ipv6_deliver(packet, next_header, field, &source, &destination);
	return error;
}

/*
 * Tells whether a device keeps a packet for a destination: an address of
 * its own (not one under detection), the all-nodes group, the
 * solicited-node group of any of its addresses (detection's too), or,
 * round the loopback device, any of the host's addresses.
 */
static int
ipv6_accepts(
	struct net_device *device,
	const struct in6_addr *destination)
{
	struct ipv6_interface *interface;
	struct in6_addr group;
	struct in6_address *entry;
	unsigned long irq;
	unsigned flags;
	unsigned index;
	int multicast;
	int accepted;
	int same;
	int local;

	/* The loopback device takes what is the host's. */
	flags = net_device_flags_get(device);
	if ((flags & NET_DEVICE_LOOPBACK) != 0U) {
		local = in6_is_loopback(destination);
		if (local)
			return 1;
		local = ipv6_address_is_local(destination);
		return local;
	}

	/* The interface's record, with IPv6 on. */
	multicast = in6_is_multicast(destination);
	accepted = 0;
	irq = spin_lock_irqsave(&ipv6_lock);

	interface = ipv6_interface_locked(device);
	if (interface != NULL && interface->enabled) {
		if (multicast) {
			/* The all-nodes group, or a solicited-node group of an address. */
			same = in6_equal(destination, &ipv6_all_nodes);
			if (same)
				accepted = 1;
			for (index = 0U; index < IN6_ADDRESSES_MAX && !accepted; index++) {
				if (!interface->addresses.entries[index].used)
					continue;
				in6_solicited_node(&group, &interface->addresses.entries[index].address);
				same = in6_equal(destination, &group);
				if (same)
					accepted = 1;
			}
		} else {
			/* An address of its own that is not under detection or taken. */
			entry = in6_address_find(&interface->addresses, destination);
			if (entry != NULL && (entry->flags & (IN6_ADDRESS_TENTATIVE | IN6_ADDRESS_DUPLICATED)) == 0U)
				accepted = 1;
		}
	}

	spin_unlock_irqrestore(&ipv6_lock, irq);

	/* Succeeded: kept or not. */
	return accepted;
}

/*
 * Walks the extension headers from offset (RFC 8200 section 4): the
 * hop-by-hop options (first only) and the destination options are
 * checked, a routing header with segments left is refused, a fragment
 * goes to the reassembly (a second fragment header is dropped), and
 * neighbor discovery that came in fragments is dropped (RFC 6980 section
 * 5).  Sets *walked to the packet the walk ended with (a reassembled
 * datagram in place of its last fragment), the upper layer's next header,
 * its offset, and the offset of the field that named it (for a Parameter
 * Problem).  Returns 0, or an errno value when the packet was consumed.
 */
static int
ipv6_extensions(
	struct packet_buf **walked,
	uint8_t *next_header,
	size_t *offset,
	size_t *field)
{
	const struct ipv6_extension_wire *extension;
	const struct ipv6_wire *header;
	struct packet_buf *packet;
	struct in6_addr destination;
	size_t length;
	unsigned count;
	unsigned type;
	int fragmented;
	int error;

	/* The destination, for the options' errors. */
	packet = *walked;
	header = (const struct ipv6_wire *)packet->data;
	kern_memcpy(destination.s6_addr, header->destination, 16U);

	/* Each extension header in turn, until an upper layer. */
	fragmented = 0;
	for (count = 0U;; count++) {
		/* An upper layer: the walk ends. */
		if (*next_header != IPV6_NEXT_HOPOPTS && *next_header != IPV6_NEXT_ROUTING &&
		    *next_header != IPV6_NEXT_FRAGMENT && *next_header != IPV6_NEXT_DSTOPTS)
			break;

		/* A second fragment header in one packet: dropped. */
		if (*next_header == IPV6_NEXT_FRAGMENT && fragmented) {
			packet_buf_free(packet);
			return EINVAL;
		}

		/* A fragment: walked on past it, or with its reassembled datagram, unless it was kept or dropped. */
		if (*next_header == IPV6_NEXT_FRAGMENT) {
			error = ipv6_fragment_input(walked, next_header, offset, field);
			if (error != 0)
				return error;
			packet = *walked;
			fragmented = 1;
			continue;
		}

		/* Hop-by-hop options come only first. */
		if (*next_header == IPV6_NEXT_HOPOPTS && count != 0U) {
			icmp6_error(packet, ICMP6_PARAMETER_PROBLEM, ICMP6_PARAMETER_NEXT_HEADER, (uint32_t)*field);
			return EINVAL;
		}

		/* The header whole. */
		if (*offset + sizeof(*extension) > packet->length) {
			packet_buf_free(packet);
			return EINVAL;
		}

		/* Its length. */
		extension = (const struct ipv6_extension_wire *)(packet->data + *offset);
		length = ((size_t)extension->length + 1U) * 8U;
		if (*offset + length > packet->length) {
			packet_buf_free(packet);
			return EINVAL;
		}

		/* Its contents: the options, or the routing header's segments left. */
		if (*next_header == IPV6_NEXT_ROUTING) {
			if (length >= 4U && packet->data[*offset + 3U] != 0U) {
				icmp6_error(packet, ICMP6_PARAMETER_PROBLEM, ICMP6_PARAMETER_HEADER, (uint32_t)(*offset + 3U));
				return EINVAL;
			}
		} else {
			error = ipv6_options(packet, *offset, length, &destination);
			if (error != 0)
				return error;
		}

		/* The next one, named by this header's first byte. */
		*next_header = extension->next_header;
		*field = *offset;
		*offset += length;
	}

	/* Neighbor discovery never comes in fragments: such a message is dropped. */
	if (fragmented &&
	    *next_header == IPV6_NEXT_ICMPV6 &&
	    *offset < packet->length) {
		type = packet->data[*offset];

		/* Router and neighbor discovery messages are the types from the router solicitation to the redirect. */
		if (type >= ICMP6_ROUTER_SOLICITATION && type <= ICMP6_REDIRECT) {
			packet_buf_free(packet);
			return EINVAL;
		}
	}

	/* Succeeded: the upper layer's header. */
	return 0;
}

/*
 * Checks the options of a hop-by-hop or destination options header
 * (RFC 8200 section 4.2): padding and the router alert are passed over,
 * an unknown option is skipped or makes the packet dropped (with a
 * Parameter Problem when its type says so).  Returns 0, or an errno
 * value when the packet was consumed.
 */
static int
ipv6_options(
	struct packet_buf *packet,
	size_t start,
	size_t length,
	const struct in6_addr *destination)
{
	size_t position;
	size_t option_length;
	unsigned type;
	unsigned action;
	int multicast;

	/* Each option after the two bytes of the header. */
	position = start + 2U;
	while (position < start + length) {
		type = packet->data[position];

		/* A single byte of padding. */
		if (type == 0U) {
			position++;
			continue;
		}

		/* An option's length, within the header. */
		if (position + 2U > start + length) {
			packet_buf_free(packet);
			return EINVAL;
		}

		/* Its length. */
		option_length = (size_t)packet->data[position + 1U] + 2U;
		if (position + option_length > start + length) {
			packet_buf_free(packet);
			return EINVAL;
		}

		/* Padding and the router alert are known; an unknown one's two high bits say what to do. */
		action = type >> 6;
		if (type != 1U && type != 5U && action != 0U) {
			multicast = in6_is_multicast(destination);
			if (action == 2U || (action == 3U && !multicast)) {
				icmp6_error(packet, ICMP6_PARAMETER_PROBLEM, ICMP6_PARAMETER_OPTION, (uint32_t)position);
				return EINVAL;
			}

			/* An unknown option the packet may not pass. */
			packet_buf_free(packet);
			return EINVAL;
		}

		/* The next option. */
		position += option_length;
	}

	/* Succeeded: the options allow the packet. */
	return 0;
}

/* Hands an upper layer's message to ICMPv6 or a registered next header, or answers that none takes it. */
static int
ipv6_deliver(
	struct packet_buf *packet,
	uint8_t next_header,
	size_t field,
	const struct in6_addr *source,
	const struct in6_addr *destination)
{
	unsigned index;
	int error;

	/* ICMPv6. */
	if (next_header == IPV6_NEXT_ICMPV6) {
		error = icmp6_input(packet, source, destination);
		return error;
	}

	/* No next header: nothing more. */
	if (next_header == IPV6_NEXT_NONE) {
		packet_buf_free(packet);
		return 0;
	}

	/* A transport. */
	for (index = 0U; index < ipv6_protocol_count; index++) {
		if (ipv6_protocols[index].next_header == next_header) {
			error = ipv6_protocols[index].input(packet, source, destination);
			return error;
		}
	}

	/* Nobody takes it (RFC 8200 section 4: a Parameter Problem pointing at the next header). */
	icmp6_error(packet, ICMP6_PARAMETER_PROBLEM, ICMP6_PARAMETER_NEXT_HEADER, (uint32_t)field);
	return 0;
}

/*
 * Puts the IPv6 header in front of a payload for device: hop_limit (0
 * for the interface's), and the path MTU, which a payload that does not
 * fit breaks (no fragment is made).  The packet is not consumed.
 * Returns 0, ENODEV, EMSGSIZE or ENOBUFS.
 */
static int
ipv6_header_push(
	struct net_device *device,
	const struct in6_addr *source,
	const struct in6_addr *destination,
	uint8_t next_header,
	unsigned hop_limit,
	struct packet_buf *packet)
{
	struct ipv6_wire *header;
	struct ipv6_link link;
	size_t payload;
	unsigned mtu;
	int error;

	/* The link values: the hop limit and the MTU (the path's may be smaller). */
	error = ipv6_link_get(device, &link);
	if (error != 0)
		return error;
	if (hop_limit == 0U)
		hop_limit = link.hop_limit;
	mtu = icmp6_path_mtu(destination, link.mtu);

	/* No fragment is made: a payload that does not fit is refused. */
	payload = packet->length;
	if (payload + IPV6_HEADER_LENGTH > mtu || payload > 0xffffU)
		return EMSGSIZE;

	/* The header. */
	header = packet_buf_push(packet, sizeof(*header));
	if (header == NULL)
		return ENOBUFS;

	/* Its fields. */
	ipv6_header_fill(header, source, destination, next_header, hop_limit, payload);
	packet->l3_offset = (uint16_t)(packet->data - packet->storage);

	/* Succeeded: the header is in place. */
	return 0;
}

/* Writes an IPv6 header's fields: no traffic class or flow label, the payload's length. */
static void
ipv6_header_fill(
	struct ipv6_wire *header,
	const struct in6_addr *source,
	const struct in6_addr *destination,
	uint8_t next_header,
	unsigned hop_limit,
	size_t payload)
{
	/* Version 6 and the rest of the fields. */
	kern_memset(header, 0, sizeof(*header));
	header->version_class_flow[0] = 0x60U;
	wire_put16(header->payload_length, (uint16_t)payload);
	header->next_header = next_header;
	header->hop_limit = (uint8_t)hop_limit;
	kern_memcpy(header->source, source->s6_addr, 16U);
	kern_memcpy(header->destination, destination->s6_addr, 16U);
}

/*
 * Finds where a unicast destination goes: the host's own address (any
 * interface: it goes round the loopback device), a route (on the given
 * device only, when one is given), or a link-local address on the given
 * device.  Sets *device (referenced; the caller releases it) and the next
 * hop.  Returns 0, ENETUNREACH or EHOSTUNREACH.
 */
static int
ipv6_next_hop(
	struct net_device **device,
	const struct in6_addr *destination,
	struct in6_addr *next_hop)
{
	const struct in6_route *route;
	struct ipv6_interface *interface;
	struct net_device *found;
	unsigned long irq;
	unsigned ifindex;
	uint64_t now;
	int linklocal;
	int direct;
	int referenced;

	/* The interface asked for, if any. */
	now = ipv6_now_ms();
	linklocal = in6_is_linklocal(destination);
	found = NULL;
	irq = spin_lock_irqsave(&ipv6_lock);

	ifindex = 0U;
	if (*device != NULL) {
		interface = ipv6_interface_locked(*device);
		if (interface != NULL)
			ifindex = interface->ifindex;
	}

	/* The route; a link-local destination on the interface asked for is on the link. */
	route = NULL;
	if (*device == NULL || ifindex != 0U)
		route = in6_route_lookup(&ipv6_routes, destination, ifindex, now);
	*next_hop = *destination;
	if (route != NULL) {
		found = route->device;
		direct = in6_is_unspecified(&route->gateway);
		if (!direct)
			*next_hop = route->gateway;
	} else if (linklocal && ifindex != 0U) {
		found = *device;
	}

	spin_unlock_irqrestore(&ipv6_lock, irq);

	/* No way there. */
	if (found == NULL)
		return ENETUNREACH;

	/* The device held for the send. */
	referenced = net_device_ref_live(found);
	if (!referenced)
		return EHOSTUNREACH;

	/* Succeeded: the device and the next hop. */
	*device = found;
	return 0;
}

/* Gives every live device its IPv6 record. */
static void
ipv6_interfaces_ensure(
	void)
{
	struct net_device *device;
	unsigned index;

	/* Each device in the registry. */
	for (index = 0U;; index++) {
		device = net_device_at_ref(index);
		if (device == NULL)
			break;
		ipv6_interface_ensure(device);
		net_device_release(device);
	}
}

/*
 * Gives a device its IPv6 record if it has none: IPv6 on, the default
 * link values, and ::1 on the loopback device.
 */
static void
ipv6_interface_ensure(
	struct net_device *device)
{
	struct ipv6_interface *interface;
	unsigned long irq;
	unsigned index;
	unsigned flags;
	uint64_t now;
	int referenced;
	int added;
	int kept;

	/* A device that has a record already, and a reference for a new one. */
	irq = spin_lock_irqsave(&ipv6_lock);

	interface = ipv6_interface_locked(device);

	spin_unlock_irqrestore(&ipv6_lock, irq);

	/* Nothing to do for one that has a record. */
	if (interface != NULL)
		return;
	referenced = net_device_ref_live(device);
	if (!referenced)
		return;

	/* A free record, unless another pass made it meanwhile. */
	now = ipv6_now_ms();
	flags = net_device_flags_get(device);
	kept = 0;
	irq = spin_lock_irqsave(&ipv6_lock);

	interface = ipv6_interface_locked(device);
	for (index = 0U; index < NET_DEVICE_MAX && interface == NULL; index++) {
		if (ipv6_interfaces[index].device != NULL)
			continue;
		interface = &ipv6_interfaces[index];
		kern_memset(interface, 0, sizeof(*interface));
		interface->device = device;
		interface->ifindex = device->ifindex;
		interface->enabled = 1;
		interface->link.hop_limit = IPV6_HOP_LIMIT_DEFAULT;
		interface->link.mtu = device->mtu;
		interface->link.reachable_ms = ND6_REACHABLE_MS;
		interface->link.retrans_ms = IN6_RETRANS_MS;
		if ((flags & NET_DEVICE_LOOPBACK) != 0U) {
			(void)in6_address_add(&interface->addresses, &ipv6_loopback, 128U, IN6_ADDRESS_NODAD,
			    IN6_LIFETIME_FOREVER, IN6_LIFETIME_FOREVER, now, &added);
		}

		/* The record keeps the reference. */
		kept = 1;
	}

	spin_unlock_irqrestore(&ipv6_lock, irq);

	/* The reference stays with a new record. */
	if (!kept)
		net_device_release(device);
}

/* Finds a device's record; the caller holds the lock. */
static struct ipv6_interface *
ipv6_interface_locked(
	const struct net_device *device)
{
	unsigned index;

	/* Each record. */
	if (device == NULL)
		return NULL;
	for (index = 0U; index < NET_DEVICE_MAX; index++) {
		if (ipv6_interfaces[index].device == device)
			return &ipv6_interfaces[index];
	}

	/* None. */
	return NULL;
}

/*
 * Moves one record on (the lock held): the carrier's return, the
 * addresses' events, a router solicitation and a listener report due.
 * Appends the work to do after the lock to work from count and returns
 * the new count.
 */
static unsigned
ipv6_interface_tick_locked(
	struct ipv6_interface *interface,
	uint64_t now_ms,
	struct ipv6_work *work,
	unsigned count)
{
	struct in6_address_event events[8];
	unsigned events_count;
	unsigned index;
	unsigned flags;
	int carrier;
	int linklocal;

	/* Nothing while IPv6 is off, and nothing to detect, solicit or report on the loopback device. */
	if (!interface->enabled)
		return count;
	flags = net_device_flags_get(interface->device);
	if ((flags & NET_DEVICE_LOOPBACK) != 0U)
		return count;

	/* The carrier came back: the addresses are checked again, and the groups reported. */
	carrier = net_device_carrier(interface->device);
	if (carrier && !interface->carrier) {
		in6_address_restart(&interface->addresses, now_ms);
		interface->report_ms = now_ms;
	}

	/* No work without carrier. */
	interface->carrier = carrier;
	if (!carrier)
		return count;

	/* The addresses' events: a solicitation to send, a link-local address made the interface's. */
	events_count = in6_address_tick(&interface->addresses, now_ms, events, 8U);
	for (index = 0U; index < events_count; index++) {
		/* A change is told. */
		if (events[index].kind != IN6_EVENT_DAD_PROBE && count < IPV6_WORK_MAX) {
			work[count].kind = IPV6_WORK_TELL;
			work[count].device = interface->device;
			work[count].address = events[index].address;
			work[count].event = events[index].kind;
			work[count].prefixlen = events[index].prefixlen;
			work[count].flags = events[index].flags;
			work[count].ifindex = interface->ifindex;
			count++;
		}

		/* An address gone takes its connected route. */
		if (events[index].kind == IN6_EVENT_EXPIRED)
			ipv6_connected_locked(interface, &events[index].address, events[index].prefixlen, 0);

		/* A solicitation for a tentative address. */
		if (events[index].kind == IN6_EVENT_DAD_PROBE && count < IPV6_WORK_MAX) {
			work[count].kind = IPV6_WORK_DAD;
			work[count].device = interface->device;
			work[count].address = events[index].address;
			count++;
		}

		/* A link-local address made the interface's: routers are solicited. */
		linklocal = in6_is_linklocal(&events[index].address);
		if (events[index].kind == IN6_EVENT_PREFERRED && linklocal) {
			interface->solicitations = ND6_RTR_SOLICITATIONS;
			interface->solicit_ms = now_ms;
		}

		/* A new address's group is reported. */
		if (events[index].kind == IN6_EVENT_DAD_PROBE && interface->report_ms == 0U)
			interface->report_ms = now_ms;
	}

	/* A router solicitation due. */
	if (interface->solicitations != 0U && now_ms >= interface->solicit_ms && count < IPV6_WORK_MAX) {
		interface->solicitations--;
		interface->solicit_ms = now_ms + ND6_RTR_SOLICITATION_MS;
		work[count].kind = IPV6_WORK_RS;
		work[count].device = interface->device;
		count++;
	}

	/* A listener report due. */
	if (interface->report_ms != 0U && now_ms >= interface->report_ms && count < IPV6_WORK_MAX) {
		interface->report_ms = 0U;
		work[count].kind = IPV6_WORK_MLD;
		work[count].device = interface->device;
		count++;
	}

	/* Succeeded: the work so far. */
	return count;
}

/*
 * Does the timer's work with the lock let go: each device held while it
 * sends, a listener report with the device's groups.
 */
static void
ipv6_work_do(
	const struct ipv6_work *work,
	unsigned count)
{
	struct in6_addr groups[IN6_ADDRESSES_MAX];
	unsigned groups_count;
	unsigned index;
	int referenced;

	/* Each piece in turn. */
	for (index = 0U; index < count; index++) {
		referenced = net_device_ref_live(work[index].device);
		if (!referenced)
			continue;

		/* What the piece sends. */
		switch (work[index].kind) {
		case IPV6_WORK_DAD:
			(void)nd6_send_solicitation(work[index].device, &work[index].address, NULL, NULL, 1);
			break;
		case IPV6_WORK_RS:
			(void)nd6_send_router_solicitation(work[index].device);
			break;
		case IPV6_WORK_MLD:
			groups_count = ipv6_groups(work[index].device, groups, IN6_ADDRESSES_MAX);
			(void)mld6_report(work[index].device, groups, groups_count);
			break;
		case IPV6_WORK_TELL:
			ipv6_tell(work[index].ifindex, work[index].event, &work[index].address, work[index].prefixlen, work[index].flags);
			break;
		default:
			break;
		}

		/* The device let go. */
		net_device_release(work[index].device);
	}
}

/*
 * Writes the groups a record listens to that are reported (the
 * solicited-node group of each address, once; the all-nodes group is
 * never reported).  Returns how many.
 */
static unsigned
ipv6_groups_locked(
	const struct ipv6_interface *interface,
	struct in6_addr *groups,
	unsigned capacity)
{
	struct in6_addr group;
	unsigned count;
	unsigned index;
	unsigned seen;
	int same;

	/* Each address's group, not twice. */
	count = 0U;
	for (index = 0U; index < IN6_ADDRESSES_MAX && count < capacity; index++) {
		if (!interface->addresses.entries[index].used)
			continue;
		in6_solicited_node(&group, &interface->addresses.entries[index].address);
		same = 0;
		for (seen = 0U; seen < count && !same; seen++)
			same = in6_equal(&groups[seen], &group);
		if (same)
			continue;
		groups[count] = group;
		count++;
	}

	/* Succeeded: the groups. */
	return count;
}

/*
 * Adds (add nonzero) or removes the connected route of an address's
 * prefix on its interface (the lock held): none for a /128; removed only
 * once no other address of the interface has the prefix.
 */
static void
ipv6_connected_locked(
	struct ipv6_interface *interface,
	const struct in6_addr *address,
	unsigned prefixlen,
	int add)
{
	struct in6_route route;
	struct in6_address *entry;
	void *changed;
	unsigned index;
	int same;

	/* A host address has no prefix to reach. */
	if (prefixlen >= 128U)
		return;

	/* Added: the prefix on the link. */
	if (add) {
		kern_memset(&route, 0, sizeof(route));
		route.destination = *address;
		route.prefixlen = prefixlen;
		route.ifindex = interface->ifindex;
		route.device = interface->device;
		route.flags = RTF_UP | RTF_CONNECTED;
		route.metric = IPV6_CONNECTED_METRIC;
		(void)in6_route_add(&ipv6_routes, &route, &changed);
		return;
	}

	/* Removed, unless another address keeps the prefix. */
	for (index = 0U; index < IN6_ADDRESSES_MAX; index++) {
		entry = &interface->addresses.entries[index];
		if (!entry->used || entry->prefixlen != prefixlen)
			continue;
		same = in6_prefix_match(&entry->address, address, prefixlen);
		if (same)
			return;
	}

	/* No other address has it. */
	(void)in6_route_delete(&ipv6_routes, address, prefixlen, interface->ifindex, &changed);
}

/* Tells an address's change on the routing sockets (RTM_ADDRINFO). */
static void
ipv6_tell(
	unsigned ifindex,
	unsigned event,
	const struct in6_addr *address,
	unsigned prefixlen,
	unsigned flags)
{
	unsigned transition;

	/* The event's transition. */
	switch (event) {
	case IN6_EVENT_PREFERRED:
		transition = RTM_ADDRINFO_PREFERRED;
		break;
	case IN6_EVENT_DUPLICATE:
		transition = RTM_ADDRINFO_DUPLICATE;
		break;
	case IN6_EVENT_DEPRECATED:
		transition = RTM_ADDRINFO_DEPRECATED;
		break;
	case IN6_EVENT_EXPIRED:
		transition = RTM_ADDRINFO_EXPIRED;
		break;
	default:
		return;
	}

	/* Told. */
	route_socket_notify_address(ifindex, address, prefixlen, transition, flags);
}
