/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/net/netutil.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>

int
netutil_ifreq(struct ifreq *request, const char *name)
{
	size_t length;
	if (request == NULL || name == NULL) { errno = EINVAL; return -1; }
	length = strlen(name);
	if (length == 0 || length >= IFNAMSIZ) { errno = EINVAL; return -1; }
	memset(request, 0, sizeof(*request));
	memcpy(request->ifr_name, name, length + 1U);
	return 0;
}

int
netutil_ifindex(int descriptor, const char *name, uint32_t *index)
{
	struct ifreq request;
	if (netutil_ifreq(&request, name) != 0 ||
	    ioctl(descriptor, SIOCGIFINDEX, &request) != 0) return -1;
	*index = request.ifr_ifindex;
	return 0;
}

int
netutil_ifname(int descriptor, uint32_t index, char *name)
{
	struct ifreq request;
	memset(&request, 0, sizeof(request));
	request.ifr_ifindex = index;
	if (ioctl(descriptor, SIOCGIFNAME, &request) != 0) return -1;
	memcpy(name, request.ifr_name, IFNAMSIZ);
	return 0;
}

int
netutil_interfaces(int descriptor, struct ifreq **requests, unsigned *count)
{
	struct ifconf config;
	struct ifreq *buffer;
	memset(&config, 0, sizeof(config));
	if (ioctl(descriptor, SIOCGIFCONF, &config) != 0) return -1;
	if (config.ifc_len == 0) { *requests = NULL; *count = 0; return 0; }
	buffer = malloc(config.ifc_len);
	if (buffer == NULL) { errno = ENOMEM; return -1; }
	config.ifc_buf = (uint64_t)(uintptr_t)buffer;
	if (ioctl(descriptor, SIOCGIFCONF, &config) != 0) {
		free(buffer); return -1;
	}
	*requests = buffer;
	*count = config.ifc_len / sizeof(*buffer);
	return 0;
}

int netutil_parse_ipv4(const char *text, struct in_addr *address)
{
	if (!inet_aton(text, address)) { errno = EINVAL; return -1; }
	return 0;
}

int
netutil_mask_prefix(struct in_addr mask, unsigned *prefix)
{
	uint32_t value = ntohl(mask.s_addr);
	unsigned bits = 0;
	while ((value & 0x80000000U) != 0) { bits++; value <<= 1; }
	if (value != 0) { errno = EINVAL; return -1; }
	*prefix = bits;
	return 0;
}

int
netutil_parse_cidr(const char *text, struct in_addr *address,
	struct in_addr *mask, unsigned *prefix)
{
	char buffer[64], *slash, *end;
	unsigned long bits;
	if (strlen(text) >= sizeof(buffer)) { errno = EINVAL; return -1; }
	strcpy(buffer, text);
	slash = strchr(buffer, '/');
	if (slash == NULL) { errno = EINVAL; return -1; }
	*slash++ = '\0';
	bits = strtoul(slash, &end, 10);
	if (*slash == '\0' || *end != '\0' || bits > 32U ||
	    !inet_aton(buffer, address)) { errno = EINVAL; return -1; }
	mask->s_addr = htonl(bits == 0 ? 0U : 0xffffffffU << (32U - bits));
	if (prefix != NULL) *prefix = (unsigned)bits;
	return 0;
}

uint64_t
netutil_monotonic_us(void)
{
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
	return (uint64_t)(uint32_t)now.tv_sec * 1000000ULL +
	    (uint32_t)now.tv_nsec / 1000U;
}

int
netutil_parse_milliseconds(const char *text, uint32_t *result)
{
	uint64_t seconds = 0, fraction = 0;
	unsigned digits = 0;
	if (text == NULL || *text < '0' || *text > '9') return -1;
	while (*text >= '0' && *text <= '9') {
		seconds = seconds * 10U + (unsigned)(*text++ - '0');
		if (seconds > 4294967U) return -1;
	}
	if (*text == '.') {
		text++;
		while (*text >= '0' && *text <= '9' && digits < 3U) {
			fraction = fraction * 10U + (unsigned)(*text++ - '0'); digits++;
		}
		if (*text >= '0' && *text <= '9') return -1;
	}
	if (*text != '\0') return -1;
	while (digits++ < 3U) fraction *= 10U;
	seconds = seconds * 1000U + fraction;
	if (seconds > 0xffffffffU) return -1;
	*result = (uint32_t)seconds;
	return 0;
}
