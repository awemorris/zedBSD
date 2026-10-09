/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The network's details for Settings (ws089-p003, KL_VERSION 11):
 * the interfaces as the kernel reports them (their addresses, hardware
 * addresses and the bytes they carried), the DNS servers of
 * /etc/resolv.conf, and the keys of the Wi-Fi networks the user saves.
 *
 * The interfaces are read with the socket ioctls ifconfig uses; the keys
 * go into the credential store "net wifi key" writes
 * (userland/base/net/wifi-store.c), which the daemon reads when it is told
 * the saved networks changed.  Nothing here speaks to the daemon or waits
 * for it.
 *
 * A wired interface (ws089-p022: not the loopback, not a radio "wlan...")
 * also says how it is configured, as net.conf names it (an interface it
 * does not name takes DHCP, as networkd's wired policy gives it), and the
 * router of the default route through it.
 */

#include "userland/desktop/libkeiland-backend/keiland-backend.h"

#include "userland/base/net/netconf.h"
#include "userland/base/net/wifi-store.h"

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <net/route.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <uapi/wlan.h>
#include <unistd.h>

/* The file the resolver reads its servers from. */
#define LINK_RESOLV_CONF "/etc/resolv.conf"

/* The longest line of the resolver's file that is read. */
#define LINK_LINE_MAX 256U

static void link_read(int descriptor, const char *name, struct kl_backend_network_link *link);
static unsigned link_wireless(int descriptor, const char *name);
static int link_request(int descriptor, const char *name, unsigned long command, struct ifreq *request);
static void link_address(const struct ifreq *request, char *text, size_t size);
static void link_wired(int descriptor, struct kl_backend_network_link *links, size_t count);
static unsigned link_wired_mode(const struct netconf *configuration, const char *name);
static void link_router(int descriptor, struct kl_backend_network_link *link);

/*
 * Copies up to capacity interfaces and returns how many there are (0 when they cannot be read).
 */
size_t
kl_backend_network_get_links(
	struct kl_backend_network_link *links,
	size_t capacity)
{
	struct ifconf config;
	struct ifreq *requests;
	size_t count;
	size_t filled;
	size_t index;
	int descriptor;
	int status;

	/* A socket to ask the kernel through. */
	descriptor = socket(AF_INET, SOCK_DGRAM, 0);
	if (descriptor < 0)
		return 0;

	/* How much room the list of interfaces needs. */
	memset(&config, 0, sizeof(config));
	status = ioctl(descriptor, SIOCGIFCONF, &config);
	if (status != 0 || config.ifc_len == 0U) {
		(void)close(descriptor);
		return 0;
	}

	/* The list itself. */
	requests = malloc(config.ifc_len);
	if (requests == NULL) {
		(void)close(descriptor);
		return 0;
	}

	/* The kernel fills it. */
	config.ifc_buf = (uint64_t)(uintptr_t)requests;
	status = ioctl(descriptor, SIOCGIFCONF, &config);
	if (status != 0) {
		free(requests);
		(void)close(descriptor);
		return 0;
	}

	/* Each interface, as many as fit. */
	count = config.ifc_len / sizeof(requests[0]);
	for (index = 0; index < count && index < capacity; index++)
		link_read(descriptor, requests[index].ifr_name, &links[index]);

	/* How the wired ones that fit are configured. */
	filled = count;
	if (filled > capacity)
		filled = capacity;
	link_wired(descriptor, links, filled);

	/* The list and the socket go. */
	free(requests);
	(void)close(descriptor);

	/* Succeeded: the number of interfaces. */
	return count;
}

/*
 * Copies up to capacity DNS servers of /etc/resolv.conf (its nameserver lines, dotted IPv4) and returns how many were copied.
 */
size_t
kl_backend_network_get_dns(
	char (*servers)[KL_BACKEND_NETWORK_ADDRESS_MAX],
	size_t capacity)
{
	char line[LINK_LINE_MAX];
	char word[32];
	char address[KL_BACKEND_NETWORK_ADDRESS_MAX + 16U];
	struct in_addr parsed;
	FILE *file;
	size_t count;
	char *read;
	int fields;
	int differs;
	int valid;

	/* A machine without the file has no servers. */
	file = fopen(LINK_RESOLV_CONF, "r");
	if (file == NULL)
		return 0;

	/* Each line that names a server, until the room is used up. */
	count = 0;
	for (;;) {
		read = fgets(line, sizeof(line), file);
		if (read == NULL || count == capacity)
			break;

		/* Only "nameserver ADDRESS" names a server. */
		fields = sscanf(line, "%31s %31s", word, address);
		if (fields != 2)
			continue;

		/* Ignores resolver options that do not name a DNS server. */
		differs = strcmp(word, "nameserver");
		if (differs != 0)
			continue;

		/* Only an IPv4 address is shown. */
		valid = inet_pton(AF_INET, address, &parsed);
		if (valid != 1)
			continue;

		/* The server. */
		(void)snprintf(servers[count], KL_BACKEND_NETWORK_ADDRESS_MAX, "%s", address);
		count++;
	}

	/* The file goes. */
	(void)fclose(file);

	/* Succeeded: the servers found. */
	return count;
}

/*
 * Saves the key of a Wi-Fi network in the user's credential store, to be joined by itself from then on.
 *
 * Returns 0 or an errno value.
 */
int
kl_backend_network_save_key(
	const char *ssid,
	const char *key)
{
	char diagnostic[WIFI_CONF_DIAGNOSTIC_MAX];
	size_t ssid_length;
	size_t key_length;
	int save_status;
	int error;

	/* An SSID of one to 32 bytes, and a key of 8 to 63 characters. */
	if (ssid == NULL || key == NULL)
		return EINVAL;

	/* Refuses an empty SSID or one too long for the credential protocol. */
	ssid_length = strlen(ssid);
	if (ssid_length == 0 || ssid_length > KL_BACKEND_NETWORK_SSID_MAX - 1U)
		return EINVAL;

	/* Refuses a key outside the saved passphrase range. */
	key_length = strlen(key);
	if (key_length < KL_BACKEND_NETWORK_KEY_MIN || key_length > KL_BACKEND_NETWORK_KEY_MAX)
		return EINVAL;

	/* The store of the user this process runs as (joined automatically from now on). */
	diagnostic[0] = '\0';
	save_status = wifi_store_set_key_for_effective_user(ssid, key, 1, diagnostic, sizeof(diagnostic));
	error = errno;

	/* The diagnostic may quote the store's lines; it is not kept. */
	wifi_conf_explicit_clear(diagnostic, sizeof(diagnostic));

	/* A store that refused reports why. */
	if (save_status != 0) {
		/* Gives a refused store operation an errno even when it supplied none. */
		if (error == 0)
			error = EIO;
		return error;
	}

	/* Succeeded: the key is saved. */
	return 0;
}

/*
 * Copies up to capacity SSIDs the user has saved keys for (as text) and returns how many there are; the keys themselves never leave the store.
 */
size_t
kl_backend_network_get_saved(
	char (*ssids)[KL_BACKEND_NETWORK_SSID_MAX],
	size_t capacity)
{
	char diagnostic[WIFI_CONF_DIAGNOSTIC_MAX];
	struct wifi_conf_model *model;
	const struct wifi_conf_profile *profile;
	size_t count;
	size_t index;
	size_t length;
	int load_status;

	/* The model is large, and holds keys: it is allocated and wiped. */
	model = malloc(sizeof(*model));
	if (model == NULL)
		return 0;

	/* Initializes the model before reading the credential store. */
	wifi_conf_model_init(model);

	/* The user's store; a missing one has no networks. */
	diagnostic[0] = '\0';
	load_status = wifi_store_load_for_effective_user(model, diagnostic, sizeof(diagnostic));
	wifi_conf_explicit_clear(diagnostic, sizeof(diagnostic));
	if (load_status != 0) {
		wifi_conf_model_clear(model);
		free(model);
		return 0;
	}

	/* Each network's SSID, as many as fit. */
	count = model->profile_count;
	for (index = 0; index < count && index < capacity; index++) {
		/* Bounds the copied public SSID without exposing its saved key. */
		profile = &model->profiles[index];
		length = profile->ssid_length;
		if (length > KL_BACKEND_NETWORK_SSID_MAX - 1U)
			length = KL_BACKEND_NETWORK_SSID_MAX - 1U;

		/* Publishes one terminated SSID in the caller's bounded list. */
		memcpy(ssids[index], profile->ssid, length);
		ssids[index][length] = '\0';
	}

	/* The keys are wiped with the model. */
	wifi_conf_model_clear(model);
	free(model);

	/* Succeeded: the number of saved networks. */
	return count;
}

/* Reads one interface's flags, addresses, hardware address, MTU and counters; what cannot be read stays empty. */
static void
link_read(
	int descriptor,
	const char *name,
	struct kl_backend_network_link *link)
{
	struct ifreq request;
	int error;

	/* The name, and nothing known yet. */
	memset(link, 0, sizeof(*link));
	(void)snprintf(link->name, sizeof(link->name), "%s", name);

	/* Up, the link there, and the loopback. */
	error = link_request(descriptor, name, SIOCGIFFLAGS, &request);
	if (error == 0) {
		/* Marks interfaces enabled by their administrative configuration. */
		if ((request.ifr_flags & IFF_UP) != 0)
			link->up = 1;

		/* Marks interfaces with a live physical link. */
		if ((request.ifr_flags & IFF_RUNNING) != 0)
			link->running = 1;

		/* Marks the local loopback interface independently of link state. */
		if ((request.ifr_flags & IFF_LOOPBACK) != 0)
			link->loopback = 1;
	}

	/* The MTU. */
	error = link_request(descriptor, name, SIOCGIFMTU, &request);
	if (error == 0 && request.ifr_mtu > 0)
		link->mtu = (unsigned)request.ifr_mtu;

	/* The hardware address. */
	error = link_request(descriptor, name, SIOCGIFHWADDR, &request);
	if (error == 0)
		memcpy(link->hardware, request.ifr_hwaddr, sizeof(link->hardware));

	/* The IPv4 address, and its netmask when it has one. */
	error = link_request(descriptor, name, SIOCGIFADDR, &request);
	if (error == 0)
		link_address(&request, link->address, sizeof(link->address));

	/* Reads a netmask only as a display attribute of an assigned address. */
	error = link_request(descriptor, name, SIOCGIFNETMASK, &request);
	if (error == 0 && link->address[0] != '\0')
		link_address(&request, link->netmask, sizeof(link->netmask));

	/* A radio: the kernel answers its Wi-Fi status, or says it has Wi-Fi but cannot answer now (BUG-284). */
	link->wireless = link_wireless(descriptor, name);

	/* The bytes received and sent, and the link's speed the driver last heard (BUG-222). */
	error = link_request(descriptor, name, SIOCGIFSTATS, &request);
	if (error == 0) {
		link->received_bytes = request.ifr_data.ifi_ibytes;
		link->sent_bytes = request.ifr_data.ifi_obytes;
		link->link_mbps = request.ifr_data.ifi_link_mbps;
	}

	/* Succeeded: the interface holds every available attribute. */
	return;
}

/*
 * Tells whether an interface is a radio (BUG-284): the kernel's Wi-Fi
 * status request (a query anyone may make) succeeds, or fails otherwise
 * than "not supported" (a Wi-Fi interface stopping); any other interface
 * answers EOPNOTSUPP.  Returns 1 or 0.
 */
static unsigned
link_wireless(
	int descriptor,
	const char *name)
{
	struct wlan_status_request status;
	int error;
	int status_error;

	/* The interface's Wi-Fi status. */
	memset(&status, 0, sizeof(status));
	(void)snprintf(status.ifr_name, sizeof(status.ifr_name), "%s", name);
	status.version = WLAN_ABI_VERSION;
	status.size = sizeof(status);
	error = ioctl(descriptor, SIOCGWLANSTATUS, &status);
	status_error = errno;

	/* Answered: a radio. */
	if (error == 0)
		return 1U;

	/* Not a radio, or gone. */
	if (status_error == EOPNOTSUPP || status_error == ENODEV || status_error == ENXIO)
		return 0U;

	/* Succeeded: a radio that cannot answer now. */
	return 1U;
}

/* Asks the kernel one thing of an interface; returns 0 or -1. */
static int
link_request(
	int descriptor,
	const char *name,
	unsigned long command,
	struct ifreq *request)
{
	int status;

	/* The request names the interface. */
	memset(request, 0, sizeof(*request));
	(void)snprintf(request->ifr_name, sizeof(request->ifr_name), "%s", name);

	/* The kernel's answer. */
	status = ioctl(descriptor, command, request);
	if (status != 0)
		return -1;

	/* Succeeded: the request holds the answer. */
	return 0;
}

/* Writes an answer's IPv4 address as dotted text, or nothing for the unset address. */
static void
link_address(
	const struct ifreq *request,
	char *text,
	size_t size)
{
	const struct sockaddr_in *address;
	const char *written;

	/* Nothing yet. */
	text[0] = '\0';

	/* The unset address is no address. */
	address = (const struct sockaddr_in *)&request->ifr_addr;
	if (address->sin_addr.s_addr == 0U)
		return;

	/* The dotted text. */
	written = inet_ntop(AF_INET, &address->sin_addr, text, (socklen_t)size);
	if (written == NULL)
		text[0] = '\0';

	/* Succeeded: the output holds the dotted address or remains empty. */
	return;
}

/*
 * Gives each wired interface of the list how it is configured and the
 * router of the default route through it (ws089-p022).
 */
static void
link_wired(
	int descriptor,
	struct kl_backend_network_link *links,
	size_t count)
{
	struct netconf *configuration;
	char error[160];
	size_t index;
	int loaded;

	/* net.conf, when it can be read (without it every wired interface takes DHCP). */
	configuration = calloc(1, sizeof(*configuration));
	loaded = -1;
	if (configuration != NULL)
		loaded = netconf_load(NETCONF_PATH, configuration, error, sizeof(error));

	/* Each wired interface. */
	for (index = 0; index < count; index++) {
		if (links[index].loopback || links[index].wireless)
			continue;
		links[index].wired_mode = KL_BACKEND_WIRED_DHCP;
		if (loaded == 0)
			links[index].wired_mode = link_wired_mode(configuration, links[index].name);
		link_router(descriptor, &links[index]);
	}

	/* The configuration read goes. */
	free(configuration);
}

/* Tells how net.conf configures an interface: DHCP, a static address, or not known (disabled). */
static unsigned
link_wired_mode(
	const struct netconf *configuration,
	const char *name)
{
	const struct netconf_interface *item;
	size_t index;
	int differs;

	/* The interface's entry. */
	for (index = 0; index < configuration->interface_count; index++) {
		item = &configuration->interfaces[index];
		differs = strcmp(item->name, name);
		if (differs != 0)
			continue;

		/* Disabled, DHCP, or a static address. */
		if (item->enabled_set && !item->enabled)
			return KL_BACKEND_WIRED_UNKNOWN;
		if (item->dhcp)
			return KL_BACKEND_WIRED_DHCP;
		if (item->address_count != 0U)
			return KL_BACKEND_WIRED_STATIC;
		return KL_BACKEND_WIRED_UNKNOWN;
	}

	/* Not named: DHCP, as the wired policy gives it. */
	return KL_BACKEND_WIRED_DHCP;
}

/* Finds the router of the default route through an interface (none: empty). */
static void
link_router(
	int descriptor,
	struct kl_backend_network_link *link)
{
	const struct sockaddr_in *destination;
	const struct sockaddr_in *mask;
	const struct sockaddr_in *gateway;
	struct rtentry route;
	struct ifreq request;
	const char *shown;
	uint32_t ifindex;
	int status;
	int error;

	/* The interface's index. */
	link->router[0] = '\0';
	error = link_request(descriptor, link->name, SIOCGIFINDEX, &request);
	if (error != 0)
		return;
	ifindex = (uint32_t)request.ifr_ifindex;

	/* Each route, until the default through this interface (the kernel says ENOENT past the last). */
	memset(&route, 0, sizeof(route));
	for (route.rt_index = 0;; route.rt_index++) {
		status = ioctl(descriptor, SIOCGRTENTRY, &route);
		if (status != 0)
			return;
		destination = (const struct sockaddr_in *)&route.rt_dst;
		mask = (const struct sockaddr_in *)&route.rt_genmask;
		if (destination->sin_addr.s_addr != 0 || mask->sin_addr.s_addr != 0 || route.rt_ifindex != ifindex)
			continue;

		/* Its router, as text. */
		gateway = (const struct sockaddr_in *)&route.rt_gateway;
		shown = inet_ntop(AF_INET, &gateway->sin_addr, link->router, sizeof(link->router));
		if (shown == NULL)
			link->router[0] = '\0';
		return;
	}
}
