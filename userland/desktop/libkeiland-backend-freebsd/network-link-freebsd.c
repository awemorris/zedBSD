/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Reads native FreeBSD interface records and selects net80211 radios.
 * Address acquisition remains the system's responsibility.
 */
#include "userland/desktop/libkeiland-backend/wpa/network-wpa.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <netinet/in.h>
#include <net80211/ieee80211_ioctl.h>
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * Merges native address and link records into bounded interface snapshots.
 */
size_t
kl_backend_network_get_links(
	struct kl_backend_network_link *links,
	size_t capacity)
{
	struct ifaddrs *addresses;
	struct ifaddrs *address;
	struct sockaddr_in *ipv4;
	struct sockaddr_dl *hardware;
	struct if_data *data;
	const char *printed;
	size_t count;
	size_t index;
	size_t bytes;
	int wireless;
	int same;
	int error;

	/* Requires caller-owned slots before acquiring native records. */
	if (links == NULL || capacity == 0)
		return 0;

	/* Acquires one consistent native snapshot without retaining kernel pointers. */
	error = getifaddrs(&addresses);
	if (error != 0)
		return 0;

	/* Empty fields mean information absent from this snapshot. */
	memset(links, 0, capacity * sizeof(*links));
	count = 0;

	/* Combines independent records without charging another slot for an address. */
	for (address = addresses;
	     address != NULL;
	     address = address->ifa_next) {
		/* Ignores records without a representable native interface name. */
		if (address->ifa_name == NULL)
			continue;

		/* Rejects truncated names that could alias another interface. */
		bytes = strlen(address->ifa_name);
		if (bytes >= KL_BACKEND_NETWORK_NAME_MAX)
			continue;

		/* Finds a previously reported interface before consuming another slot. */
		for (index = 0; index < count; index++) {
			same = strcmp(links[index].name, address->ifa_name);
			if (same == 0)
				break;
		}

		/* Leaves excess interfaces out without discarding existing address updates. */
		if (index == count) {
			if (count == capacity)
				continue;
			memcpy(links[index].name, address->ifa_name, bytes + 1);
			count++;
		}

		/* Publishes native administrative and running flags as public Boolean fields. */
		if ((address->ifa_flags & IFF_UP) != 0)
			links[index].up = 1;

		/* Records driver readiness before applying independent carrier information. */
		if ((address->ifa_flags & IFF_RUNNING) != 0)
			links[index].running = 1;

		/* Keeps loopback out of the externally connected interface selection. */
		if ((address->ifa_flags & IFF_LOOPBACK) != 0)
			links[index].loopback = 1;

		/* Records without an address cannot describe a link or IPv4 endpoint. */
		if (address->ifa_addr == NULL)
			continue;

		/* Native AF_LINK carries MAC bytes and full-width traffic counters. */
		if (address->ifa_addr->sa_family == AF_LINK) {
			hardware = (struct sockaddr_dl *)address->ifa_addr;
			bytes = offsetof(struct sockaddr_dl, sdl_data) + hardware->sdl_nlen + sizeof(links[index].hardware);

			/* Reads MAC bytes only when the native record includes all six. */
			if (hardware->sdl_alen >= sizeof(links[index].hardware) && bytes <= hardware->sdl_len)
				memcpy(links[index].hardware, LLADDR(hardware), sizeof(links[index].hardware));

			/* Native getifaddrs owns if_data for the lifetime of this address list. */
			data = address->ifa_data;
			if (data != NULL) {
				links[index].mtu = data->ifi_mtu;
				links[index].received_bytes = data->ifi_ibytes;
				links[index].sent_bytes = data->ifi_obytes;
			}
		} else if (address->ifa_addr->sa_family == AF_INET) {
			/* Copies one IPv4 address using the public textual representation. */
			ipv4 = (struct sockaddr_in *)address->ifa_addr;
			printed = inet_ntop(AF_INET, &ipv4->sin_addr, links[index].address, sizeof(links[index].address));
			if (printed == NULL)
				links[index].address[0] = '\0';

			/* A missing netmask remains empty instead of inventing a prefix. */
			if (address->ifa_netmask != NULL) {
				ipv4 = (struct sockaddr_in *)address->ifa_netmask;
				printed = inet_ntop(AF_INET, &ipv4->sin_addr, links[index].netmask, sizeof(links[index].netmask));
				if (printed == NULL)
					links[index].netmask[0] = '\0';
			}
		}
	}

	/* Carrier-down overrides driver-running regardless of address record order. */
	for (address = addresses;
	     address != NULL;
	     address = address->ifa_next) {
		/* Only native link records carry the independent carrier state. */
		if (address->ifa_name == NULL || address->ifa_addr == NULL)
			continue;

		/* Leaves other protocol records out of the native link-state interpretation. */
		if (address->ifa_addr->sa_family != AF_LINK)
			continue;

		/* An unknown carrier retains the driver's existing running indication. */
		data = address->ifa_data;
		if (data == NULL)
			continue;

		/* A positively down carrier cannot carry an addressed connection. */
		if (data->ifi_link_state != LINK_STATE_DOWN)
			continue;

		/* Applies the carrier state only to the matching bounded public slot. */
		for (index = 0; index < count; index++) {
			same = strcmp(links[index].name, address->ifa_name);
			if (same == 0)
				links[index].running = 0;
		}
	}

	/* Releases the snapshot after copying all public fields. */
	freeifaddrs(addresses);

	/* Each radio, as net80211 answers it (BUG-284). */
	for (index = 0; index < count; index++) {
		wireless = kwpa_wireless(links[index].name);
		if (wireless)
			links[index].wireless = 1U;
	}

	/* Succeeded: reports native interfaces without retaining kernel resources. */
	return count;
}

/*
 * Changes one native interface's administrative up flag while retaining both flag halves.
 */
int
kwpa_radio(
	const char *interface,
	unsigned enabled)
{
	struct ifreq request;
	size_t bytes;
	int descriptor;
	int error;

	/* Rejects unsupported requests before touching an interface. */
	if (interface == NULL || enabled > 1)
		return EINVAL;

	/* Refuses a truncated name that might target another interface. */
	bytes = strlen(interface);
	if (bytes == 0 || bytes >= sizeof(request.ifr_name))
		return EINVAL;

	/* Opens an inquiry socket with no inherited descriptor ownership. */
	descriptor = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (descriptor < 0)
		return errno;

	/* Reads the complete native flag pair before changing its low administrative bit. */
	memset(&request, 0, sizeof(request));
	memcpy(request.ifr_name, interface, bytes + 1);
	error = ioctl(descriptor, SIOCGIFFLAGS, &request);
	if (error != 0) {
		error = errno;
		(void)close(descriptor);
		return error;
	}

	/* Preserves driver and high-word flags while setting the requested state. */
	if (enabled != 0) {
		request.ifr_flags |= IFF_UP;
	} else {
		request.ifr_flags &= ~IFF_UP;
	}

	/* Reports the native permission or device failure instead of simulating success. */
	error = ioctl(descriptor, SIOCSIFFLAGS, &request);
	if (error != 0) {
		error = errno;
		(void)close(descriptor);
		return error;
	}

	/* Releases the inquiry descriptor after installing the native flags. */
	(void)close(descriptor);

	/* Succeeded: the requested administrative flag is installed. */
	return 0;
}

/*
 * Identifies a net80211 interface through a native SSID query.
 */
int
kwpa_wireless(
	const char *interface)
{
	struct ieee80211req request;
	char ssid[32];
	size_t bytes;
	int descriptor;
	int error;

	/* A missing name cannot identify an interface. */
	if (interface == NULL)
		return 0;

	/* Refuses names outside the native request's fixed storage. */
	bytes = strlen(interface);
	if (bytes == 0 || bytes >= sizeof(request.i_name))
		return 0;

	/* A failed query socket means wireless classification is unavailable. */
	descriptor = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (descriptor < 0)
		return 0;

	/* Uses net80211 capability rather than assuming names or Ethernet link types. */
	memset(&request, 0, sizeof(request));
	memcpy(request.i_name, interface, bytes + 1);
	request.i_type = IEEE80211_IOC_SSID;
	request.i_len = sizeof(ssid);
	request.i_data = ssid;
	error = ioctl(descriptor, SIOCG80211, &request);
	(void)close(descriptor);

	/* A wired or unsupported interface cannot answer the native SSID request. */
	if (error != 0)
		return 0;

	/* Succeeded: the native interface implements the net80211 radio contract. */
	return 1;
}

/*
 * Names the FreeBSD supplicant's normal control directory.
 */
const char *
kwpa_control_directory(
	void)
{
	/* Succeeded: uses the native system service's control endpoint directory. */
	return "/var/run/wpa_supplicant";
}

/*
 * Packs a bounded native Unix-domain address including its byte-sized extent.
 */
int
kwpa_socket_address(
	struct sockaddr_un *address,
	const char *path,
	socklen_t *length)
{
	size_t bytes;

	/* Requires endpoint storage and a pathname before inspecting either. */
	if (address == NULL || path == NULL || length == NULL)
		return EINVAL;

	/* Refuses paths that cannot carry a NUL within the native record. */
	bytes = strlen(path);
	if (bytes == 0 || bytes >= sizeof(address->sun_path))
		return ENAMETOOLONG;

	/* Copies this exact endpoint and publishes its native bind/connect extent. */
	memset(address, 0, sizeof(*address));
	address->sun_family = AF_UNIX;
	memcpy(address->sun_path, path, bytes + 1);
	*length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + bytes + 1);
	address->sun_len = (unsigned char)*length;

	/* Succeeded: the native address carries its own bounded extent. */
	return 0;
}

/*
 * Requires both administrative readiness and native carrier for address selection.
 */
int
kwpa_link_usable(
	const struct kl_backend_network_link *link)
{
	/* Disabled or carrier-down interfaces cannot carry an external connection. */
	if (link == NULL)
		return 0;

	/* An address retained while administratively down is not connected. */
	if (link->up == 0)
		return 0;

	/* A running driver with a positively down carrier remains disconnected. */
	if (link->running == 0)
		return 0;

	/* Succeeded: native administrative and carrier readiness permit selection. */
	return 1;
}
