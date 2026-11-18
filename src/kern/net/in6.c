/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * IPv6 address tests (in6.h; ws130-p002): the kinds and scopes of an
 * address, prefixes, the solicited-node group and its link address, and
 * RFC 6724's default policy table.  Byte loops only, so the file builds
 * the same in the kernel and on the host.
 */

#include "in6.h"

/*
 * One row of RFC 6724's default policy table (section 2.1): a prefix, its
 * length, its precedence and its label.
 */
struct in6_policy {
	uint8_t prefix[16];
	unsigned prefixlen;
	unsigned precedence;
	unsigned label;
};

/* The default policy table, the longest prefixes first. */
static const struct in6_policy in6_policies[] = {
	{ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 }, 128U, 50U, 0U },
	{ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 0, 0, 0, 0 }, 96U, 35U, 4U },
	{ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, 96U, 1U, 3U },
	{ { 0x20, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, 32U, 5U, 5U },
	{ { 0x20, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, 16U, 30U, 2U },
	{ { 0x3f, 0xfe, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, 16U, 1U, 12U },
	{ { 0xfe, 0xc0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, 10U, 1U, 11U },
	{ { 0xfc, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, 7U, 3U, 13U },
	{ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, 0U, 40U, 1U },
};

static const struct in6_policy *in6_policy_find(const struct in6_addr *address);

/* Tells whether two addresses are the same. */
int
in6_equal(
	const struct in6_addr *left,
	const struct in6_addr *right)
{
	unsigned index;

	/* Each byte. */
	for (index = 0U; index < 16U; index++) {
		if (left->s6_addr[index] != right->s6_addr[index])
			return 0;
	}

	/* The same. */
	return 1;
}

/* Tells whether an address is ::. */
int
in6_is_unspecified(
	const struct in6_addr *address)
{
	unsigned index;

	/* Every byte is zero. */
	for (index = 0U; index < 16U; index++) {
		if (address->s6_addr[index] != 0U)
			return 0;
	}

	/* The unspecified address. */
	return 1;
}

/* Tells whether an address is ::1. */
int
in6_is_loopback(
	const struct in6_addr *address)
{
	unsigned index;

	/* Fifteen zeros. */
	for (index = 0U; index < 15U; index++) {
		if (address->s6_addr[index] != 0U)
			return 0;
	}

	/* And a one. */
	if (address->s6_addr[15] != 1U)
		return 0;
	return 1;
}

/* Tells whether an address is a multicast group (ff00::/8). */
int
in6_is_multicast(
	const struct in6_addr *address)
{
	/* Its first byte. */
	if (address->s6_addr[0] != 0xffU)
		return 0;
	return 1;
}

/* Tells whether an address is link-local unicast (fe80::/10). */
int
in6_is_linklocal(
	const struct in6_addr *address)
{
	/* Its first ten bits. */
	if (address->s6_addr[0] != 0xfeU)
		return 0;
	if ((address->s6_addr[1] & 0xc0U) != 0x80U)
		return 0;
	return 1;
}

/* Tells whether an address is an IPv4 address mapped (::ffff:0:0/96). */
int
in6_is_v4mapped(
	const struct in6_addr *address)
{
	unsigned index;

	/* Ten zeros. */
	for (index = 0U; index < 10U; index++) {
		if (address->s6_addr[index] != 0U)
			return 0;
	}

	/* Then two 0xff. */
	if (address->s6_addr[10] != 0xffU || address->s6_addr[11] != 0xffU)
		return 0;
	return 1;
}

/*
 * Gives an address's scope: a multicast group's own, the interface's for
 * the loopback, the link's for link-local unicast, the site's for the old
 * site-local, and the global one for the rest (RFC 6724 section 3.1).
 */
unsigned
in6_scope(
	const struct in6_addr *address)
{
	int test;

	/* A group carries its scope in its second byte. */
	test = in6_is_multicast(address);
	if (test)
		return address->s6_addr[1] & 0x0fU;

	/* The loopback and link-local unicast are the link's (the loopback's is the interface's, ranked with them). */
	test = in6_is_loopback(address);
	if (test)
		return IN6_SCOPE_LINK;
	test = in6_is_linklocal(address);
	if (test)
		return IN6_SCOPE_LINK;

	/* The deprecated site-local unicast (fec0::/10). */
	if (address->s6_addr[0] == 0xfeU && (address->s6_addr[1] & 0xc0U) == 0xc0U)
		return IN6_SCOPE_SITE;

	/* Everything else. */
	return IN6_SCOPE_GLOBAL;
}

/* Gives how many leading bits two addresses share. */
unsigned
in6_common_prefix(
	const struct in6_addr *left,
	const struct in6_addr *right)
{
	unsigned index;
	unsigned bits;
	unsigned difference;

	/* Whole bytes alike, then the bits of the first that differs. */
	bits = 0U;
	for (index = 0U; index < 16U; index++) {
		difference = (unsigned)(left->s6_addr[index] ^ right->s6_addr[index]);
		if (difference == 0U) {
			bits += 8U;
			continue;
		}

		/* The bits before the first that differs. */
		while ((difference & 0x80U) == 0U) {
			bits++;
			difference <<= 1;
		}

		/* Nothing after it counts. */
		break;
	}

	/* Succeeded: the length of the common prefix. */
	return bits;
}

/* Tells whether an address is in a prefix. */
int
in6_prefix_match(
	const struct in6_addr *address,
	const struct in6_addr *prefix,
	unsigned prefixlen)
{
	unsigned common;

	/* A prefix longer than an address matches nothing. */
	if (prefixlen > 128U)
		return 0;

	/* The common leading bits cover the prefix. */
	common = in6_common_prefix(address, prefix);
	if (common < prefixlen)
		return 0;
	return 1;
}

/* Keeps only an address's first prefixlen bits. */
void
in6_prefix_mask(
	struct in6_addr *out,
	const struct in6_addr *address,
	unsigned prefixlen)
{
	unsigned index;
	unsigned bits;

	/* Each byte: whole, part or none. */
	for (index = 0U; index < 16U; index++) {
		bits = 0U;
		if (prefixlen > index * 8U)
			bits = prefixlen - index * 8U;
		if (bits >= 8U) {
			out->s6_addr[index] = address->s6_addr[index];
		} else {
			out->s6_addr[index] = (uint8_t)(address->s6_addr[index] & (uint8_t)(0xff00U >> bits));
		}
	}
}

/* Gives an address's solicited-node group, ff02::1:ffXX:XXXX (RFC 4291 section 2.7.1). */
void
in6_solicited_node(
	struct in6_addr *out,
	const struct in6_addr *address)
{
	unsigned index;

	/* ff02:: with the group's fixed part. */
	for (index = 0U; index < 16U; index++)
		out->s6_addr[index] = 0U;
	out->s6_addr[0] = 0xffU;
	out->s6_addr[1] = 0x02U;
	out->s6_addr[11] = 0x01U;
	out->s6_addr[12] = 0xffU;

	/* The address's last 24 bits. */
	out->s6_addr[13] = address->s6_addr[13];
	out->s6_addr[14] = address->s6_addr[14];
	out->s6_addr[15] = address->s6_addr[15];
}

/* Gives a group's Ethernet address, 33:33 and its last 32 bits (RFC 2464 section 7). */
void
in6_multicast_link(
	uint8_t link[6],
	const struct in6_addr *group)
{
	link[0] = 0x33U;
	link[1] = 0x33U;
	link[2] = group->s6_addr[12];
	link[3] = group->s6_addr[13];
	link[4] = group->s6_addr[14];
	link[5] = group->s6_addr[15];
}

/* Gives an address's precedence in RFC 6724's default policy table. */
unsigned
in6_policy_precedence(
	const struct in6_addr *address)
{
	const struct in6_policy *policy;

	/* The longest prefix that matches. */
	policy = in6_policy_find(address);
	return policy->precedence;
}

/* Gives an address's label in RFC 6724's default policy table. */
unsigned
in6_policy_label(
	const struct in6_addr *address)
{
	const struct in6_policy *policy;

	/* The longest prefix that matches. */
	policy = in6_policy_find(address);
	return policy->label;
}

/* Finds the policy of an address: the first row (the longest prefix) that matches; ::/0 always does. */
static const struct in6_policy *
in6_policy_find(
	const struct in6_addr *address)
{
	struct in6_addr prefix;
	size_t index;
	size_t count;
	unsigned byte;
	int match;

	/* Each row, in order. */
	count = sizeof(in6_policies) / sizeof(in6_policies[0]);
	for (index = 0U; index + 1U < count; index++) {
		for (byte = 0U; byte < 16U; byte++)
			prefix.s6_addr[byte] = in6_policies[index].prefix[byte];
		match = in6_prefix_match(address, &prefix, in6_policies[index].prefixlen);
		if (match)
			return &in6_policies[index];
	}

	/* The default row. */
	return &in6_policies[count - 1U];
}
