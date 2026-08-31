/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD userland netutil component.
 */

#include "userland/base/net/netutil.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>

/*
 * Implements the netutil ifreq operation.
 */
int
netutil_ifreq(
	struct ifreq *request,
	const char *name)
{
	size_t length;

	/* Handles the request availability. */
	if (request == NULL || name == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	length = strlen(name);

	/* Checks the current data length. */
	if (length == 0 || length >= IFNAMSIZ) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	memset(request, 0, sizeof(*request));
	memcpy(request->ifr_name, name, length + 1U);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the netutil ifindex operation.
 */
int
netutil_ifindex(
	int descriptor,
	const char *name,
	uint32_t *index)
{
	struct ifreq request;

	/* Handles a failed netutil ifreq operation. */
	if (netutil_ifreq(&request, name) != 0 ||
	    ioctl(descriptor, SIOCGIFINDEX, &request) != 0)

		/* Reports operation failure. */
		return -1;
	*index = request.ifr_ifindex;
	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the netutil ifname operation.
 */
int
netutil_ifname(
	int descriptor,
	uint32_t index,
	char *name)
{
	struct ifreq request;

	memset(&request, 0, sizeof(request));
	request.ifr_ifindex = index;

	/* Handles a failed ioctl operation. */
	if (ioctl(descriptor, SIOCGIFNAME, &request) != 0)
		return -1;
	memcpy(name, request.ifr_name, IFNAMSIZ);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the netutil interfaces operation.
 */
int
netutil_interfaces(
	int descriptor,
	struct ifreq **requests,
	unsigned *count)
{
	struct ifconf config;
	struct ifreq *buffer;

	memset(&config, 0, sizeof(config));

	/* Handles a failed ioctl operation. */
	if (ioctl(descriptor, SIOCGIFCONF, &config) != 0)
		return -1;

	/* Handles the config condition. */
	if (config.ifc_len == 0) {
		*requests = NULL;
		*count = 0;
		/* Reports successful completion. */
		return 0;
	}
	buffer = malloc(config.ifc_len);

	/* Handles the buffer availability. */
	if (buffer == NULL) {
		errno = ENOMEM;

		/* Reports operation failure. */
		return -1;
	}
	config.ifc_buf = (uint64_t)(uintptr_t)buffer;

	/* Handles a failed ioctl operation. */
	if (ioctl(descriptor, SIOCGIFCONF, &config) != 0) {
		free(buffer);

		/* Reports operation failure. */
		return -1;
	}
	*requests = buffer;
	*count = config.ifc_len / sizeof(*buffer);
	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the netutil parse ipv4 operation.
 */
int
netutil_parse_ipv4(
	const char *text,
	struct in_addr *address)
{
	/* Handles a failed inet aton operation. */
	if (!inet_aton(text, address)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the netutil mask prefix operation.
 */
int
netutil_mask_prefix(
	struct in_addr mask,
	unsigned *prefix)
{
	uint32_t value;
	unsigned bits;

	/* Continue while the operation condition remains true. */
	value = ntohl(mask.s_addr);
	bits = 0;
	while ((value & 0x80000000U) != 0) {
		bits++;
		value <<= 1;
	}

	/* Validates the current value. */
	if (value != 0) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	*prefix = bits;
	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the netutil parse cidr operation.
 */
int
netutil_parse_cidr(
	const char *text,
	struct in_addr *address,
	struct in_addr *mask,
	unsigned *prefix)
{
	char buffer[64], *slash, *end;
	unsigned long bits;

	/* Handles a failed strlen operation. */
	if (strlen(text) >= sizeof(buffer)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	strcpy(buffer, text);
	slash = strchr(buffer, '/');

	/* Handles the slash availability. */
	if (slash == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	*slash++ = '\0';
	bits = strtoul(slash, &end, 10);

	/* Handles a failed inet aton operation. */
	if (*slash == '\0' || *end != '\0' || bits > 32U ||
	    !inet_aton(buffer, address)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	mask->s_addr = htonl(bits == 0 ? 0U : 0xffffffffU << (32U - bits));

	/* Handles the prefix availability. */
	if (prefix != NULL)
		*prefix = (unsigned)bits;
	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the netutil monotonic us operation.
 */
uint64_t
netutil_monotonic_us(
	void)
{
	struct timespec now;

	/* Handles a failed clock gettime operation. */
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0;

	/* Returns the computed result. */
	return (uint64_t)(uint32_t)now.tv_sec * 1000000ULL +
	       (uint32_t)now.tv_nsec / 1000U;
}

/*
 * Implements the netutil parse milliseconds operation.
 */
int
netutil_parse_milliseconds(
	const char *text,
	uint32_t *result)
{
	uint64_t seconds, fraction;
	unsigned digits;

	seconds = 0;
	fraction = 0;
	digits = 0;

	/* Handles the text availability. */
	if (text == NULL || *text < '0' || *text > '9')
		return -1;

	/* Continue while the operation condition remains true. */
	while (*text >= '0' && *text <= '9') {
		seconds = seconds * 10U + (unsigned)(*text++ - '0');

		/* Handles the seconds condition. */
		if (seconds > 4294967U)
			return -1;
	}

	/* Validates the current text. */
	if (*text == '.') {
		text++;

		/* Continue while the operation condition remains true. */
		while (*text >= '0' && *text <= '9' && digits < 3U) {
			fraction = fraction * 10U + (unsigned)(*text++ - '0');
			digits++;
		}

		/* Validates the current text. */
		if (*text >= '0' && *text <= '9')
			return -1;
	}

	/* Validates the current text. */
	if (*text != '\0')
		return -1;

	/* Continue while the operation condition remains true. */
	while (digits++ < 3U)
		fraction *= 10U;
	seconds = seconds * 1000U + fraction;

	/* Handles the seconds condition. */
	if (seconds > 0xffffffffU)
		return -1;
	*result = (uint32_t)seconds;
	/* Reports successful completion. */
	return 0;
}
