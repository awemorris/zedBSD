/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The pure parts of networkd's IPv6 stateless address autoconfiguration
 * (ws130-p006): reading a Router Advertisement, the stable interface
 * identifier of RFC 7217, an address from a prefix, a temporary
 * address's lifetimes and when the next one is made (RFC 8981), and the
 * router chosen for the default route (ws177-p045).
 */

#include "userland/base/networkd/slaac.h"
#include "userland/base/common/sha256.h"

#include <string.h>

/* The advertisement's type, its fixed part's length, and its options' types. */
#define SLAAC_TYPE_RA		134U
#define SLAAC_RA_FIXED		16U
#define SLAAC_OPTION_PREFIX	3U
#define SLAAC_OPTION_RDNSS	25U
#define SLAAC_OPTION_DNSSL	31U

/* A prefix option's flags: on-link and autonomous. */
#define SLAAC_PREFIX_ONLINK	0x80U
#define SLAAC_PREFIX_AUTONOMOUS	0x40U

static uint16_t slaac_read16(const uint8_t *bytes);
static uint32_t slaac_read32(const uint8_t *bytes);
static void slaac_prefix(const uint8_t *option, struct slaac_ra *ra);
static void slaac_rdnss(const uint8_t *option, size_t length, struct slaac_ra *ra);
static void slaac_dnssl(const uint8_t *option, size_t length, struct slaac_ra *ra);
static int slaac_label_valid(const uint8_t *label, size_t length);

/*
 * Reads a Router Advertisement (the ICMPv6 message from its type byte).
 * Returns 0, or -1 when it is not one or an option runs past its end.
 */
int
slaac_parse(
	const uint8_t *message,
	size_t length,
	struct slaac_ra *ra)
{
	const uint8_t *option;
	size_t offset;
	size_t size;

	/* An advertisement with its fixed part. */
	memset(ra, 0, sizeof(*ra));
	if (length < SLAAC_RA_FIXED || message[0] != SLAAC_TYPE_RA)
		return -1;
	ra->flags = message[5] & (SLAAC_RA_MANAGED | SLAAC_RA_OTHER);
	ra->router_lifetime = slaac_read16(message + 6);

	/* Each option: its type and its length in units of eight bytes. */
	offset = SLAAC_RA_FIXED;
	while (offset + 2U <= length) {
		option = message + offset;
		size = (size_t)option[1] * 8U;
		if (size == 0U || offset + size > length)
			return -1;

		/* The options networkd acts on. */
		if (option[0] == SLAAC_OPTION_PREFIX && size == 32U)
			slaac_prefix(option, ra);
		else if (option[0] == SLAAC_OPTION_RDNSS && size >= 24U)
			slaac_rdnss(option, size, ra);
		else if (option[0] == SLAAC_OPTION_DNSSL && size >= 16U)
			slaac_dnssl(option, size, ra);
		offset += size;
	}

	/* Succeeded. */
	return 0;
}

/*
 * Makes the stable interface identifier of RFC 7217 section 5: the low 64
 * bits of SHA-256 over the prefix, the interface's name, the network's
 * identity (a Wi-Fi network's name, "" for a cable), the duplicate address
 * detection counter and the secret.
 */
void
slaac_stable_iid(
	const uint8_t *secret,
	size_t secret_length,
	const struct in6_addr *prefix,
	const char *interface,
	const char *network,
	unsigned dad_counter,
	uint8_t *iid)
{
	struct command_sha256_context context;
	uint8_t digest[32];
	uint8_t counter[4];

	/* The inputs in their order. */
	counter[0] = (uint8_t)(dad_counter >> 24);
	counter[1] = (uint8_t)(dad_counter >> 16);
	counter[2] = (uint8_t)(dad_counter >> 8);
	counter[3] = (uint8_t)dad_counter;
	command_sha256_init(&context);
	(void)command_sha256_update(&context, prefix->s6_addr, 8U);
	(void)command_sha256_update(&context, (const uint8_t *)interface, strlen(interface));
	(void)command_sha256_update(&context, (const uint8_t *)network, strlen(network));
	(void)command_sha256_update(&context, counter, sizeof(counter));
	(void)command_sha256_update(&context, secret, secret_length);
	command_sha256_final(&context, digest);

	/* Succeeded: the digest's last 64 bits. */
	memcpy(iid, digest + 24, 8U);
}

/* Puts a 64-bit prefix and an interface identifier together. */
void
slaac_address(
	const struct in6_addr *prefix,
	const uint8_t *iid,
	struct in6_addr *address)
{
	/* The prefix's 64 bits, then the identifier's. */
	memcpy(address->s6_addr, prefix->s6_addr, 8U);
	memcpy(address->s6_addr + 8, iid, 8U);
}

/*
 * Gives a temporary address's lifetimes (RFC 8981 section 3.4): no longer
 * than the prefix's, and at most two days valid and one day preferred less
 * the desynchronization.
 */
void
slaac_temporary_lifetimes(
	uint32_t valid,
	uint32_t preferred,
	uint32_t desync,
	uint32_t *temporary_valid,
	uint32_t *temporary_preferred)
{
	uint32_t longest;

	/* Valid: the prefix's, at most two days. */
	*temporary_valid = valid;
	if (*temporary_valid > SLAAC_TEMPORARY_VALID)
		*temporary_valid = SLAAC_TEMPORARY_VALID;

	/* Preferred: the prefix's, at most a day less the desynchronization. */
	longest = SLAAC_TEMPORARY_PREFERRED;
	if (desync < longest)
		longest -= desync;
	*temporary_preferred = preferred;
	if (*temporary_preferred > longest)
		*temporary_preferred = longest;
}

/*
 * Gives a temporary address's lifetimes at an age (seconds since it was
 * made; RFC 8981 section 3.4): as for a new one, and no longer than two
 * days valid and a day less the desynchronization preferred from when it
 * was made, so that advertisements do not keep it past them.
 */
void
slaac_temporary_aged(
	uint32_t valid,
	uint32_t preferred,
	uint32_t desync,
	uint64_t age,
	uint32_t *temporary_valid,
	uint32_t *temporary_preferred)
{
	uint64_t longest;
	uint64_t left;

	/* The lifetimes of a new one. */
	slaac_temporary_lifetimes(valid, preferred, desync, temporary_valid, temporary_preferred);

	/* Valid: what is left of two days. */
	left = 0;
	if (age < SLAAC_TEMPORARY_VALID)
		left = SLAAC_TEMPORARY_VALID - age;
	if (*temporary_valid > left)
		*temporary_valid = (uint32_t)left;

	/* Preferred: what is left of a day less the desynchronization, and no longer than valid. */
	longest = SLAAC_TEMPORARY_PREFERRED;
	if (desync < longest)
		longest -= desync;
	left = 0;
	if (age < longest)
		left = longest - age;
	if (*temporary_preferred > left)
		*temporary_preferred = (uint32_t)left;
	if (*temporary_preferred > *temporary_valid)
		*temporary_preferred = *temporary_valid;
}

/* Gives a temporary address's desynchronization (DESYNC_FACTOR, RFC 8981 section 3.8) from a random number. */
uint32_t
slaac_temporary_desync(
	uint32_t random)
{
	/* Between 0 and MAX_DESYNC_FACTOR. */
	return random % (SLAAC_TEMPORARY_DESYNC_MAX + 1U);
}

/*
 * Gives the seconds after a temporary address was made at which the next
 * one is made: REGEN_ADVANCE before its longest preferred lifetime runs
 * out (RFC 8981 section 3.4, step 6).
 */
uint64_t
slaac_temporary_regenerate(
	uint32_t desync)
{
	uint64_t longest;

	/* A day less the desynchronization. */
	longest = SLAAC_TEMPORARY_PREFERRED;
	if (desync < longest)
		longest -= desync;

	/* Succeeded: less the advance. */
	if (longest <= SLAAC_TEMPORARY_REGEN_ADVANCE)
		return 0;
	return longest - SLAAC_TEMPORARY_REGEN_ADVANCE;
}

/*
 * Chooses the router of the default route: one whose lifetime has not run
 * out, of the interface with the lowest rank; between equals the current
 * one (no change for nothing), else the first.  Returns its index, or -1
 * when there is none.
 */
int
slaac_router_choose(
	const struct slaac_router *routers,
	unsigned count,
	uint64_t now,
	int current)
{
	unsigned index;
	int best;

	/* Each router alive. */
	best = -1;
	for (index = 0; index < count; index++) {
		if (!routers[index].used || routers[index].expires <= now)
			continue;

		/* The first, a better rank, or the current one among equals. */
		if (best < 0) {
			best = (int)index;
		} else if (routers[index].rank < routers[best].rank) {
			best = (int)index;
		} else if (routers[index].rank == routers[best].rank && (int)index == current) {
			best = (int)index;
		}
	}

	/* The choice. */
	return best;
}

/* Reads a 16-bit number in network order. */
static uint16_t
slaac_read16(
	const uint8_t *bytes)
{
	/* The high byte first. */
	return (uint16_t)((uint16_t)bytes[0] << 8 | bytes[1]);
}

/* Reads a 32-bit number in network order. */
static uint32_t
slaac_read32(
	const uint8_t *bytes)
{
	/* The high byte first. */
	return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 | (uint32_t)bytes[2] << 8 | bytes[3];
}

/* Keeps a prefix option: its length, flags, lifetimes and prefix; a full list drops it. */
static void
slaac_prefix(
	const uint8_t *option,
	struct slaac_ra *ra)
{
	struct slaac_prefix *prefix;

	/* Room for it. */
	if (ra->prefix_count == SLAAC_PREFIXES_MAX)
		return;

	/* Succeeded: kept. */
	prefix = &ra->prefixes[ra->prefix_count++];
	prefix->length = option[2];
	prefix->onlink = (option[3] & SLAAC_PREFIX_ONLINK) != 0U;
	prefix->autonomous = (option[3] & SLAAC_PREFIX_AUTONOMOUS) != 0U;
	prefix->valid = slaac_read32(option + 4);
	prefix->preferred = slaac_read32(option + 8);
	memcpy(prefix->prefix.s6_addr, option + 16, 16U);
}

/* Keeps an RDNSS option's servers and their lifetime, three at most. */
static void
slaac_rdnss(
	const uint8_t *option,
	size_t length,
	struct slaac_ra *ra)
{
	size_t offset;

	/* The lifetime, then the addresses after the first eight bytes. */
	ra->dns_lifetime = slaac_read32(option + 4);
	for (offset = 8U; offset + 16U <= length; offset += 16U) {
		if (ra->dns_count == SLAAC_DNS_MAX)
			return;
		memcpy(ra->dns[ra->dns_count++].s6_addr, option + offset, 16U);
	}
}

/*
 * Keeps a DNSSL option's names after any kept before, separated by spaces, as
 * far as they fit; a name with a label that is not a host name's is
 * dropped whole.
 */
static void
slaac_dnssl(
	const uint8_t *option,
	size_t length,
	struct slaac_ra *ra)
{
	size_t offset;
	size_t used;
	size_t start;
	unsigned label;
	int dropping;
	int valid;

	/* The lifetime, then the names in DNS's label form after the first eight bytes; a zero label ends each. */
	ra->search_lifetime = slaac_read32(option + 4);
	used = strlen(ra->search);
	if (used != 0U && used + 1U < SLAAC_SEARCH_MAX)
		ra->search[used++] = ' ';
	start = used;
	dropping = 0;
	offset = 8U;
	while (offset < length) {
		label = option[offset++];

		/* The end of a name: a space before the next, which starts there; padding of zeros ends the list. */
		if (label == 0U) {
			if (used != 0U && ra->search[used - 1U] != ' ' && used + 1U < SLAAC_SEARCH_MAX)
				ra->search[used++] = ' ';
			start = used;
			dropping = 0;
			continue;
		}

		/* A label past the option's end: the name, and what follows, dropped. */
		if (label > 63U || offset + label > length) {
			used = start;
			break;
		}

		/* A label not a host name's, or no room for it: the whole name dropped (ws177-p046), the next one read. */
		valid = slaac_label_valid(option + offset, label);
		if (!valid || used + label + 2U >= SLAAC_SEARCH_MAX) {
			used = start;
			dropping = 1;
		}

		/* A name being dropped: its labels passed over. */
		if (dropping) {
			offset += label;
			continue;
		}

		/* A label, after a dot when the name has one already. */
		if (used != 0U && ra->search[used - 1U] != ' ')
			ra->search[used++] = '.';
		memcpy(ra->search + used, option + offset, label);
		used += label;
		offset += label;
	}

	/* Succeeded: no space at the end. */
	while (used != 0U && ra->search[used - 1U] == ' ')
		used--;
	ra->search[used] = '\0';
}

/* Tells whether a label is a host name's: letters, digits, '-' and '_' only (nothing else reaches resolv.conf). */
static int
slaac_label_valid(
	const uint8_t *label,
	size_t length)
{
	size_t index;
	uint8_t letter;

	/* Each byte. */
	for (index = 0; index < length; index++) {
		letter = label[index];
		if ((letter >= 'a' && letter <= 'z') || (letter >= 'A' && letter <= 'Z') || (letter >= '0' && letter <= '9'))
			continue;
		if (letter == '-' || letter == '_')
			continue;
		return 0;
	}

	/* Succeeded. */
	return 1;
}
