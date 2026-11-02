/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Reads shared Unix resolver configuration and maintains supplicant profiles.
 */

#include "network-wpa.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int network_ack(struct kwpa_socket *connection, const char *command);
static unsigned network_prefix6(const struct sockaddr *netmask);

/*
 * Adds an IPv6 address of getifaddrs to an interface as "ADDRESS/LENGTH",
 * its length counted from the netmask: the global ones are kept before
 * the link-local ones, a global one taking the place of a link-local one
 * when the interface has no room left.
 */
void
kwpa_link_inet6(
	struct kl_backend_network_link *link,
	const struct sockaddr *address,
	const struct sockaddr *netmask)
{
	const struct sockaddr_in6 *inet6;
	char text[INET6_ADDRSTRLEN];
	const char *written;
	unsigned slot;
	unsigned index;
	int differs;
	int local;

	/* Only an IPv6 address. */
	if (link == NULL || address == NULL || address->sa_family != AF_INET6)
		return;
	inet6 = (const struct sockaddr_in6 *)address;
	written = inet_ntop(AF_INET6, &inet6->sin6_addr, text, sizeof(text));
	if (written == NULL)
		return;
	local = IN6_IS_ADDR_LINKLOCAL(&inet6->sin6_addr) ? 1 : 0;

	/* A link-local one after the others; a global one before the first link-local one. */
	slot = link->inet6_count;
	if (!local) {
		for (slot = 0; slot < link->inet6_count; slot++) {
			differs = strncmp(link->inet6[slot], "fe80:", 5U);
			if (differs == 0)
				break;
		}
	}

	/* No room left: a link-local one is dropped, a global one drops the last link-local one. */
	if (link->inet6_count >= KL_BACKEND_NETWORK_INET6_COUNT) {
		if (local || slot >= link->inet6_count)
			return;
		link->inet6_count--;
	}

	/* The ones after the slot move down, and the address takes it. */
	for (index = link->inet6_count; index > slot; index--)
		memcpy(link->inet6[index], link->inet6[index - 1U], sizeof(link->inet6[0]));
	(void)snprintf(link->inet6[slot], sizeof(link->inet6[0]), "%s/%u", text, network_prefix6(netmask));
	link->inet6_count++;
}

/*
 * Copies dotted IPv4 resolver addresses from the system configuration.
 */
size_t
kl_backend_network_get_dns(
	char (*servers)[KL_BACKEND_NETWORK_ADDRESS_MAX],
	size_t capacity)
{
	FILE *file;
	char line[256];
	char word[32];
	char address[64];
	char *read_line;
	struct in_addr ipv4;
	size_t count;
	int fields;
	int differs;
	int parsed;

	/* Avoids opening configuration when the caller has no space. */
	if (servers == NULL || capacity == 0)
		return 0;

	/* An unreadable resolver configuration has no reported servers. */
	file = fopen("/etc/resolv.conf", "r");
	if (file == NULL)
		return 0;

	/* Copies only nameserver lines holding a valid dotted IPv4 address. */
	count = 0;
	while (count < capacity) {
		/* Reads the next resolver entry without retaining the file buffer. */
		read_line = fgets(line, sizeof(line), file);
		if (read_line == NULL)
			break;

		/* Rejects incomplete configuration entries. */
		fields = sscanf(line, "%31s %63s", word, address);
		if (fields != 2)
			continue;

		/* Ignores unrelated resolver directives. */
		differs = strcmp(word, "nameserver");
		if (differs != 0)
			continue;

		/* Rejects IPv6 and malformed IPv4 entries for this public interface. */
		parsed = inet_pton(AF_INET, address, &ipv4);
		if (parsed != 1)
			continue;

		/* Publishes the textual address in the caller's next slot. */
		(void)snprintf(servers[count], KL_BACKEND_NETWORK_ADDRESS_MAX, "%s", address);
		count++;
	}

	/* Releases the resolver file after the bounded scan. */
	(void)fclose(file);

	/* Succeeded: reports how many server addresses were copied. */
	return count;
}

/*
 * Saves an exact SSID and escaped passphrase without enabling the profile.
 */
int
kl_backend_network_save_key(
	const char *ssid,
	const char *key)
{
	struct kwpa_socket connection;
	char reply[KWPA_REPLY_MAX];
	char command[256];
	char encoded[KL_BACKEND_NETWORK_SSID_MAX * 2];
	char quoted[KL_BACKEND_NETWORK_KEY_MAX * 2 + 1];
	char *end;
	const char *digits = "0123456789abcdef";
	size_t ssid_length;
	size_t key_length;
	size_t index;
	size_t copied;
	unsigned id;
	unsigned long number;
	unsigned char byte;
	int error;

	/* Reject out-of-contract data before creating a credential profile. */
	if (ssid == NULL || key == NULL)
		return EINVAL;

	/* Validates byte bounds before allocating any daemon profile. */
	ssid_length = strlen(ssid);
	key_length = strlen(key);
	if (ssid_length == 0 ||
	    ssid_length >= KL_BACKEND_NETWORK_SSID_MAX ||
	    key_length < KL_BACKEND_NETWORK_KEY_MIN ||
	    key_length > KL_BACKEND_NETWORK_KEY_MAX)
		return EINVAL;

	/* Hex SSIDs cannot introduce quoting or control-interface commands. */
	for (index = 0; index < ssid_length; index++) {
		byte = (unsigned char)ssid[index];
		encoded[index * 2] = digits[byte >> 4];
		encoded[index * 2 + 1] = digits[byte & 15];
	}

	/* Printable passphrases escape the two quote-sensitive characters. */
	encoded[ssid_length * 2] = '\0';
	copied = 0;
	for (index = 0; index < key_length; index++) {
		/* Control bytes cannot become a second daemon command. */
		byte = (unsigned char)key[index];
		if (byte < 32 || byte > 126)
			return EINVAL;

		/* Escapes only the printable characters meaningful inside daemon quotes. */
		if (byte == '"' || byte == '\\')
			quoted[copied++] = '\\';
		quoted[copied++] = (char)byte;
	}

	/* This operation owns its socket independently of all existing watches. */
	quoted[copied] = '\0';
	error = kwpa_open(&connection, NULL);
	if (error != 0)
		return error;

	/* Finds existing profile identities on this operation's independent connection. */
	error = kwpa_call(&connection, "LIST_NETWORKS", reply, sizeof(reply));
	if (error != 0) {
		kwpa_close(&connection);
		return error;
	}

	/* Existing profiles keep their identity; new profiles start disabled. */
	error = kwpa_profile(reply, ssid, &id);
	if (error == ENOENT) {
		/* Allocates an initially disabled profile only when no exact SSID exists. */
		error = kwpa_call(&connection, "ADD_NETWORK", reply, sizeof(reply));
		if (error != 0) {
			kwpa_close(&connection);
			return error;
		}

		/* Validate the returned numeric ID before constructing another command. */
		errno = 0;
		number = strtoul(reply, &end, 10);
		if (errno != 0 ||
		    end == reply ||
		    (*end != '\n' && *end != '\0') ||
		    number > 0x7fffffffUL) {
			kwpa_close(&connection);
			return EPROTO;
		}

		/* The bounded ID is safe to format into subsequent daemon requests. */
		id = (unsigned)number;
		error = 0;
	}

	/* A malformed listing cannot authorize modification of another profile. */
	if (error != 0) {
		kwpa_close(&connection);
		return error;
	}

	/* The profile remains disabled until the explicit JOIN request. */
	(void)snprintf(command, sizeof(command), "SET_NETWORK %u ssid %s", id, encoded);
	error = network_ack(&connection, command);
	if (error != 0) {
		kwpa_close(&connection);
		return error;
	}

	/* Supplicant parses the escaped printable passphrase within quotes. */
	(void)snprintf(command, sizeof(command), "SET_NETWORK %u psk \"%s\"", id, quoted);
	error = network_ack(&connection, command);
	if (error != 0) {
		kwpa_close(&connection);
		return error;
	}

	/* Persistence failure is reported even though the daemon retains its memory. */
	error = network_ack(&connection, "SAVE_CONFIG");
	kwpa_close(&connection);
	if (error != 0)
		return error;

	/* Succeeded: saved credentials exist without a new connection attempt. */
	return 0;
}

/*
 * Copies decoded saved SSIDs from an independent bounded daemon query.
 */
size_t
kl_backend_network_get_saved(
	char (*ssids)[KL_BACKEND_NETWORK_SSID_MAX],
	size_t capacity)
{
	struct kwpa_socket connection;
	char reply[KWPA_REPLY_MAX];
	char *line;
	char *save;
	char *tab;
	char *end;
	size_t count;
	int error;

	/* A caller with no slots needs no daemon connection. */
	if (ssids == NULL || capacity == 0)
		return 0;

	/* Acquires a dedicated peer rather than sharing a watch's outstanding command. */
	error = kwpa_open(&connection, NULL);
	if (error != 0)
		return 0;

	/* Copies the whole profile listing before closing its private connection. */
	error = kwpa_call(&connection, "LIST_NETWORKS", reply, sizeof(reply));
	kwpa_close(&connection);
	if (error != 0)
		return 0;

	/* The heading is not a saved network. */
	count = 0;
	line = strtok_r(reply, "\n", &save);
	if (line == NULL)
		return 0;

	/* Copy complete SSID fields, leaving profile flags inside the daemon. */
	while (count < capacity) {
		/* An exhausted reply cannot supply another credential name. */
		line = strtok_r(NULL, "\n", &save);
		if (line == NULL)
			break;

		/* Requires a complete identity field before decoding the SSID. */
		tab = strchr(line, '\t');
		if (tab == NULL)
			continue;

		/* Requires the SSID's trailing delimiter before trimming another field. */
		tab++;
		end = strchr(tab, '\t');
		if (end == NULL)
			continue;

		/* Publishes only nonempty decoded names in the bounded caller slots. */
		*end = '\0';
		kwpa_decode(tab, ssids[count], KL_BACKEND_NETWORK_SSID_MAX);
		if (ssids[count][0] != '\0')
			count++;
	}

	/* Succeeded: the caller holds a bounded credential-name list. */
	return count;
}

/*
 * Refreshes connection kind from addressed interfaces without inventing DHCP.
 */
void
kwpa_links(
	struct kl_backend_network_state *state)
{
	struct kl_backend_network_link links[KL_BACKEND_NETWORK_LINKS_MAX];
	size_t count;
	size_t index;
	int wireless;
	int usable;

	/* A supplicant authentication alone does not imply an acquired address. */
	state->connected = 0;
	state->kind = KL_BACKEND_NETWORK_NONE;
	state->interface[0] = '\0';
	state->wired[0] = '\0';
	count = kl_backend_network_get_links(links, KL_BACKEND_NETWORK_LINKS_MAX);

	/* Wired reachability remains visible even while a Wi-Fi connection is chosen. */
	for (index = 0; index < count; index++) {
		/* Only addressed external interfaces can carry connectivity. */
		if (links[index].loopback != 0 ||
		    links[index].address[0] == '\0')
			continue;

		/* Uses the selected native backend's link/carrier contract. */
		usable = kwpa_link_usable(&links[index]);
		if (usable == 0)
			continue;

		/* Distinguishes native wireless devices from addressed wired links. */
		wireless = kwpa_wireless(links[index].name);
		if (wireless != 0) {
			/* A WiFi address alone does not prove a currently authenticated connection. */
			if (state->wifi != KL_BACKEND_WIFI_CONNECTED)
				continue;

			/* An authenticated addressed radio takes precedence over wired selection. */
			state->kind = KL_BACKEND_NETWORK_WIFI;
			state->connected = 1;
			(void)snprintf(state->interface, sizeof(state->interface), "%s", links[index].name);
		} else {
			/* Keeps the available wired interface visible even while WiFi is selected. */
			(void)snprintf(state->wired, sizeof(state->wired), "%s", links[index].name);

			/* Wired connectivity supplies the selected path when WiFi has none. */
			if (state->kind != KL_BACKEND_NETWORK_WIFI) {
				state->kind = KL_BACKEND_NETWORK_WIRED;
				state->connected = 1;
				(void)snprintf(state->interface, sizeof(state->interface), "%s", links[index].name);
			}
		}
	}

	/* Succeeded: address acquisition is reflected independently of the radio. */
	return;
}

/* Validates an independent credential command's full acknowledgement. */
static int
network_ack(
	struct kwpa_socket *connection,
	const char *command)
{
	char reply[KWPA_REPLY_MAX];
	int error;

	/* Each command consumes its reply before a later command is sent. */
	error = kwpa_call(connection, command, reply, sizeof(reply));
	if (error != 0)
		return error;

	/* A syntactically complete FAIL reply still rejects the credential change. */
	error = kwpa_ok(reply);
	if (error != 0)
		return error;

	/* Succeeded: the daemon accepted this exact credential operation. */
	return 0;
}

/* Counts the leading ones of an IPv6 netmask (128 when there is none). */
static unsigned
network_prefix6(
	const struct sockaddr *netmask)
{
	const struct sockaddr_in6 *mask;
	unsigned length;
	unsigned byte;
	unsigned bit;

	/* A missing netmask is a host's address. */
	if (netmask == NULL || netmask->sa_family != AF_INET6)
		return 128U;
	mask = (const struct sockaddr_in6 *)netmask;

	/* The ones from the top, until the first zero. */
	length = 0U;
	for (byte = 0U; byte < 16U; byte++) {
		for (bit = 0U; bit < 8U; bit++) {
			if ((mask->sin6_addr.s6_addr[byte] & (0x80U >> bit)) == 0U)
				return length;
			length++;
		}
	}

	/* Succeeded: every bit is one. */
	return length;
}
