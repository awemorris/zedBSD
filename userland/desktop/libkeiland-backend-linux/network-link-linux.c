/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Reads Linux interface metadata and packs native supplicant endpoints.
 * Address acquisition remains the system's responsibility.
 */
#include "userland/desktop/libkeiland-backend/wpa/network-wpa.h"
#include <inttypes.h>
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static uint64_t link_counter(const char *interface, const char *field);

/*
 * Merges address and hardware records into bounded interface snapshots.
 */
size_t
kl_backend_network_get_links(
	struct kl_backend_network_link *links,
	size_t capacity)
{
	struct ifaddrs *addresses;
	struct ifaddrs *address;
	struct sockaddr_in *ipv4;
	struct sockaddr_ll *hardware;
	struct ifreq inquiry;
	size_t count;
	size_t index;
	int descriptor;
	int wireless;
	int error;
	int same;
	const char *printed;

	/* The caller must provide actual slots before any interface is read. */
	if (links == NULL || capacity == 0)
		return 0;

	/* Acquires all address records before copying caller-owned slots. */
	error = getifaddrs(&addresses);
	if (error != 0)
		return 0;

	/* Empty fields describe unavailable information in this snapshot. */
	memset(links, 0, capacity * sizeof(*links));
	count = 0;

	/* IPv4 and packet addresses are separate records for the same interface. */
	for (address = addresses;
	     address != NULL;
	     address = address->ifa_next) {
		/* Records without an interface name cannot identify a public slot. */
		if (address->ifa_name == NULL)
			continue;

		/* Reuses an existing slot for another address of the same interface. */
		for (index = 0; index < count; index++) {
			same = strcmp(links[index].name, address->ifa_name);
			if (same == 0)
				break;
		}

		/* New interfaces consume one slot; extra addresses consume no new slots. */
		if (index == count) {
			/* Leaves excess interfaces outside the caller's bounded storage. */
			if (count == capacity)
				continue;

			/* Publishes the new interface identity before its address fields. */
			(void)snprintf(links[index].name, sizeof(links[index].name), "%s", address->ifa_name);
			count++;
		}

		/* Translate flag meaning explicitly rather than storing bit masks. */
		if ((address->ifa_flags & IFF_UP) != 0)
			links[index].up = 1;

		/* Reports the driver's independent running indication. */
		if ((address->ifa_flags & IFF_RUNNING) != 0)
			links[index].running = 1;

		/* Keeps loopback out of external connection selection. */
		if ((address->ifa_flags & IFF_LOOPBACK) != 0)
			links[index].loopback = 1;

		/* Missing addresses supply flags alone. */
		if (address->ifa_addr == NULL)
			continue;

		/* The public API carries one IPv4 address and netmask per interface. */
		if (address->ifa_addr->sa_family == AF_INET) {
			ipv4 = (struct sockaddr_in *)address->ifa_addr;
			printed = inet_ntop(AF_INET, &ipv4->sin_addr, links[index].address, sizeof(links[index].address));
			if (printed == NULL)
				links[index].address[0] = '\0';

			/* An absent netmask stays empty rather than inventing an address prefix. */
			if (address->ifa_netmask != NULL) {
				ipv4 = (struct sockaddr_in *)address->ifa_netmask;
				printed = inet_ntop(AF_INET, &ipv4->sin_addr, links[index].netmask, sizeof(links[index].netmask));
				if (printed == NULL)
					links[index].netmask[0] = '\0';
			}
		} else if (address->ifa_addr->sa_family == AF_PACKET) {
			/* Copies only complete Ethernet hardware addresses. */
			hardware = (struct sockaddr_ll *)address->ifa_addr;
			if (hardware->sll_halen >= sizeof(links[index].hardware))
				memcpy(links[index].hardware, hardware->sll_addr, sizeof(links[index].hardware));
		}
	}

	/* Address storage is no longer needed once all records have been copied. */
	freeifaddrs(addresses);

	/* A failed optional socket leaves traffic counters available without MTU. */
	descriptor = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);

	/* Statistics and MTU are independent of whether an IPv4 address exists. */
	for (index = 0; index < count; index++) {
		links[index].received_bytes = link_counter(links[index].name, "rx_bytes");
		links[index].sent_bytes = link_counter(links[index].name, "tx_bytes");

		/* A radio, as the kernel's interface tree tells it (BUG-284). */
		wireless = kwpa_wireless(links[index].name);
		if (wireless)
			links[index].wireless = 1U;

		/* Missing inquiry access must not prevent reporting other link fields. */
		if (descriptor >= 0) {
			memset(&inquiry, 0, sizeof(inquiry));
			(void)snprintf(inquiry.ifr_name, sizeof(inquiry.ifr_name), "%s", links[index].name);
			error = ioctl(descriptor, SIOCGIFMTU, &inquiry);
			if (error == 0 && inquiry.ifr_mtu > 0)
				links[index].mtu = (unsigned)inquiry.ifr_mtu;
		}
	}

	/* The optional inquiry socket is the only retained kernel resource. */
	if (descriptor >= 0)
		(void)close(descriptor);

	/* Succeeded: every reported entry belongs to the caller's capacity. */
	return count;
}

/*
 * Changes only the selected Linux radio's administrative up flag.
 */
int
kwpa_radio(
	const char *interface,
	unsigned enabled)
{
	struct ifreq request;
	int descriptor;
	int error;

	/* The kernel enforces CAP_NET_ADMIN rather than pretending user success. */
	descriptor = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (descriptor < 0)
		return errno;

	/* Reads the installed flags before changing the administrative bit. */
	memset(&request, 0, sizeof(request));
	(void)snprintf(request.ifr_name, sizeof(request.ifr_name), "%s", interface);
	error = ioctl(descriptor, SIOCGIFFLAGS, &request);
	if (error != 0) {
		error = errno;
		(void)close(descriptor);
		return error;
	}

	/* Preserve every flag other than the requested administrative state. */
	if (enabled != 0) {
		request.ifr_flags |= IFF_UP;
	} else {
		request.ifr_flags &= ~IFF_UP;
	}

	/* Report the actual permission or device failure to the user request. */
	error = ioctl(descriptor, SIOCSIFFLAGS, &request);
	if (error != 0) {
		error = errno;
		(void)close(descriptor);
		return error;
	}

	/* The inquiry socket owns no persistent radio state. */
	(void)close(descriptor);

	/* Succeeded: the requested administrative flag is installed. */
	return 0;
}

/*
 * Identifies Linux Wi-Fi from kernel sysfs topology.
 */
int
kwpa_wireless(
	const char *interface)
{
	char path[256];
	struct stat status;
	int error;

	/* Wireless extensions expose a wireless directory on older drivers. */
	(void)snprintf(path, sizeof(path), "/sys/class/net/%s/wireless", interface);
	error = stat(path, &status);
	if (error == 0)
		return 1;

	/* Modern mac80211 drivers expose their PHY even without wireless extensions. */
	(void)snprintf(path, sizeof(path), "/sys/class/net/%s/phy80211", interface);
	error = stat(path, &status);
	if (error != 0)
		return 0;

	/* Succeeded: the interface belongs to a wireless PHY. */
	return 1;
}

/*
 * Names the Linux supplicant's normal control directory.
 */
const char *
kwpa_control_directory(
	void)
{
	/* Succeeded: daemon configuration retains its existing Linux path. */
	return "/run/wpa_supplicant";
}

/*
 * Packs a bounded Linux Unix-domain endpoint without retaining its pathname.
 */
int
kwpa_socket_address(
	struct sockaddr_un *address,
	const char *path,
	socklen_t *length)
{
	size_t bytes;

	/* Requires storage and a pathname before inspecting either. */
	if (address == NULL || path == NULL || length == NULL)
		return EINVAL;

	/* Refuses paths that cannot carry their terminating NUL. */
	bytes = strlen(path);
	if (bytes == 0 || bytes >= sizeof(address->sun_path))
		return ENAMETOOLONG;

	/* Copies only this endpoint's extent into the native socket record. */
	memset(address, 0, sizeof(*address));
	address->sun_family = AF_UNIX;
	memcpy(address->sun_path, path, bytes + 1);
	*length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + bytes + 1);

	/* Succeeded: bind and connect can use this exact native address extent. */
	return 0;
}

/*
 * Preserves Linux's existing administrative-up address selection contract.
 */
int
kwpa_link_usable(
	const struct kl_backend_network_link *link)
{
	/* An absent or administratively disabled link cannot carry a connection. */
	if (link == NULL || link->up == 0)
		return 0;

	/* Succeeded: addressed Linux links retain their existing selection behavior. */
	return 1;
}

/* Reads one optional sysfs statistic as a bounded unsigned counter. */
static uint64_t
link_counter(
	const char *interface,
	const char *field)
{
	char path[256];
	uint64_t bytes;
	FILE *file;
	int parsed;

	/* Missing statistics are reported as zero rather than inventing traffic. */
	(void)snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/%s", interface, field);
	file = fopen(path, "r");
	if (file == NULL)
		return 0;

	/* Copies a full-width counter before releasing its optional file. */
	parsed = fscanf(file, "%" SCNu64, &bytes);
	(void)fclose(file);

	/* Malformed optional statistics cannot supply an initialized counter. */
	if (parsed != 1)
		return 0;

	/* Succeeded: the kernel's counter is copied without retaining its file. */
	return (uint64_t)bytes;
}
