/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Multicast listener reports (ws130-p002; RFC 3810, MLDv2, as a host).
 *
 * A switch that snoops MLD forwards a group's packets only to the ports
 * that reported it; without a report for the solicited-node groups,
 * neighbor solicitations would not reach the host and duplicate address
 * detection and address resolution would fail.  So the groups the host
 * listens to (the solicited-node group of each address; the all-nodes
 * group is never reported) are reported when an address is added or the
 * link comes back (a state change, CHANGE_TO_EXCLUDE), and again in
 * answer to a query (the current state, MODE_IS_EXCLUDE).  A report goes
 * to ff02::16 with the hop limit 1 and the router alert option.
 */

#include "ipv6.h"

#include "kern/net/net-device.h"
#include "kern/net/packet-buf.h"
#include "internal.h"
#include "wire.h"
#include <kern/kcrt.h>

#include <uapi/errno.h>

/* The record types (RFC 3810 section 5.2.12). */
#define MLD6_MODE_IS_EXCLUDE		2U
#define MLD6_CHANGE_TO_EXCLUDE		4U

/* The router alert's value for MLD (RFC 2711). */
#define MLD6_ROUTER_ALERT		0U

/* The hop-by-hop header a report carries: next header, length, router alert, two bytes of padding. */
#define MLD6_HOP_BY_HOP_LENGTH		8U

/* The MLDv2 listeners group, ff02::16. */
static const struct in6_addr mld6_routers = { { { 0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x16 } } };

static int mld6_send(struct net_device *device, const struct in6_addr *groups, unsigned count, unsigned record_type);

/*
 * Receives an MLD message (packet->data is the ICMPv6 message); a query
 * from a router on the link is answered with the device's groups, other
 * hosts' reports are ignored.  The packet is consumed.
 */
void
mld6_input(
	struct packet_buf *packet,
	const struct in6_addr *source,
	const struct in6_addr *destination)
{
	const struct icmp6_wire *message;
	struct in6_addr groups[IN6_ADDRESSES_MAX];
	struct net_device *device;
	unsigned count;
	int linklocal;

	/* A query from a router on the link. */
	(void)destination;
	message = (const struct icmp6_wire *)packet->data;
	linklocal = in6_is_linklocal(source);
	if (message->type != ICMP6_MLD_QUERY || !linklocal || packet->length < sizeof(struct mld6_query_wire)) {
		packet_buf_free(packet);
		return;
	}

	/* Answered with the current state of every group. */
	device = packet->device;
	net_device_ref(device);
	packet_buf_free(packet);
	count = ipv6_groups(device, groups, IN6_ADDRESSES_MAX);
	if (count != 0U)
		(void)mld6_send(device, groups, count, MLD6_MODE_IS_EXCLUDE);
	net_device_release(device);
}

/* Reports a device's groups as a state change (an address added, the link back).  Returns 0 or an errno value. */
int
mld6_report(
	struct net_device *device,
	const struct in6_addr *groups,
	unsigned count)
{
	int error;

	/* Nothing to report. */
	if (count == 0U)
		return 0;

	/* The report. */
	error = mld6_send(device, groups, count, MLD6_CHANGE_TO_EXCLUDE);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Sends an MLDv2 report of groups, each a record of the type without
 * sources: from the device's link-local address (or :: while it has
 * none), with the router alert.
 */
static int
mld6_send(
	struct net_device *device,
	const struct in6_addr *groups,
	unsigned count,
	unsigned record_type)
{
	struct mld6_report_wire *message;
	struct mld6_record_wire *record;
	struct packet_buf *packet;
	struct in6_addr source;
	uint8_t *hop_by_hop;
	size_t length;
	uint16_t sum;
	unsigned index;
	int error;

	/* The source: the link-local address, or :: (RFC 3810 section 5.2.13). */
	error = ipv6_source_select(device, &mld6_routers, &source);
	if (error != 0)
		kern_memset(&source, 0, sizeof(source));

	/* The report and its records. */
	length = sizeof(*message) + (size_t)count * sizeof(*record);
	packet = packet_buf_alloc(PACKET_BUF_DEFAULT_HEADROOM);
	message = NULL;
	if (packet != NULL)
		message = packet_buf_append(packet, length);
	if (message == NULL) {
		if (packet != NULL)
			packet_buf_free(packet);
		return ENOBUFS;
	}

	/* The report's fields. */
	kern_memset(message, 0, length);
	message->header.type = ICMP6_MLD2_REPORT;
	wire_put16(message->header.data + 2, (uint16_t)count);
	record = (struct mld6_record_wire *)(message + 1);
	for (index = 0U; index < count; index++) {
		record[index].type = (uint8_t)record_type;
		kern_memcpy(record[index].group, groups[index].s6_addr, 16U);
	}

	/* The checksum over the report (the hop-by-hop header is not the upper layer's). */
	sum = net_checksum_pseudo6(source.s6_addr, mld6_routers.s6_addr, IPV6_NEXT_ICMPV6, packet->data, packet->length);
	wire_put16(message->header.checksum, sum);

	/* The hop-by-hop header with the router alert. */
	hop_by_hop = packet_buf_push(packet, MLD6_HOP_BY_HOP_LENGTH);
	if (hop_by_hop == NULL) {
		packet_buf_free(packet);
		return ENOBUFS;
	}

	/* Its next header, its length, the router alert and padding. */
	hop_by_hop[0] = IPV6_NEXT_ICMPV6;
	hop_by_hop[1] = 0U;
	hop_by_hop[2] = 5U;
	hop_by_hop[3] = 2U;
	hop_by_hop[4] = 0U;
	hop_by_hop[5] = MLD6_ROUTER_ALERT;
	hop_by_hop[6] = 1U;
	hop_by_hop[7] = 0U;

	/* Sent to the listeners' group. */
	error = ipv6_output(device, &source, &mld6_routers, IPV6_NEXT_HOPOPTS, IPV6_HOP_LIMIT_MLD, packet);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}
