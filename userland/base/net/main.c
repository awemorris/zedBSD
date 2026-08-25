/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/net/protocol.h"
#include "userland/base/service/service-config.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define NET_INTERFACE_LIMIT 16
#define NET_DNS_LIMIT 8

static int
write_all(int descriptor, const char *buffer, size_t length)
{
	size_t offset = 0;
	while (offset < length) {
		ssize_t count =
		    write(descriptor, buffer + offset, length - offset);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			return -1;
		offset += (size_t)count;
	}
	return 0;
}

static int
backend(const char *operation, const char *operands, int display)
{
	struct sockaddr_un address;
	char request[NETWORKD_REQUEST_MAX], response[NETWORKD_RESPONSE_MAX];
	size_t used = 0;
	int descriptor = -1;
	ssize_t count;

	descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
	if (descriptor < 0)
		goto unavailable;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strcpy(address.sun_path, NETWORKD_SOCKET);
	if (connect(descriptor, (struct sockaddr *)&address, sizeof(address)) !=
	    0)
		goto unavailable;
	if (snprintf(request, sizeof(request), "%s %s%s%s\n",
		     NETWORKD_PROTOCOL_VERSION, operation,
		     operands != NULL && *operands != '\0' ? " " : "",
		     operands != NULL ? operands : "") >=
		(int)sizeof(request) ||
	    write_all(descriptor, request, strlen(request)) != 0)
		goto unavailable;
	(void)shutdown(descriptor, SHUT_WR);
	while (used + 1U < sizeof(response) &&
	       (count = read(descriptor, response + used,
			     sizeof(response) - used - 1U)) != 0) {
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0)
			goto unavailable;
		used += (size_t)count;
	}
	close(descriptor);
	descriptor = -1;
	response[used] = '\0';
	if (strncmp(response, NETWORKD_PROTOCOL_VERSION " OK", 5) == 0 &&
	    (response[5] == '\n' || response[5] == ' ')) {
		const char *payload = response + 6;
		if (display && *payload != '\0' &&
		    write_all(STDOUT_FILENO, payload, strlen(payload)) != 0)
			return 1;
		return 0;
	}
	if (strncmp(response, NETWORKD_PROTOCOL_VERSION " ERR ", 7) == 0)
		fprintf(stderr, "net: %s", response + 7);
	else
		fprintf(stderr, "net: malformed networkd response\n");
	return 1;

unavailable:
	fprintf(stderr, "net: networkd is unavailable: %s\n", strerror(errno));
	if (descriptor >= 0)
		close(descriptor);
	return 1;
}

static int
interface_name_valid(const char *name)
{
	size_t index, length;
	if (name == NULL || (length = strlen(name)) == 0 || length >= IFNAMSIZ)
		return 0;
	for (index = 0; index < length; index++)
		if (!isalnum((unsigned char)name[index]) &&
		    name[index] != '_' && name[index] != '-')
			return 0;
	return 1;
}

static int
decimal_timeout(const char *text, unsigned *result)
{
	char *end;
	unsigned long value;
	if (text == NULL || *text == '\0')
		return -1;
	value = strtoul(text, &end, 10);
	if (*end != '\0' || value < 1U || value > 3600U)
		return -1;
	*result = (unsigned)value;
	return 0;
}

static int
send_up(const char *name)
{
	return backend("UP", name, 0);
}

static int
send_dhcp(const char *name, unsigned timeout)
{
	char operands[96];
	if (snprintf(operands, sizeof(operands), "%s %u", name, timeout) >=
	    (int)sizeof(operands))
		return 1;
	return backend("DHCP", operands, 0);
}

static int
send_static(const char *name, const char *address, const char *mask)
{
	char operands[160];
	if (snprintf(operands, sizeof(operands), "%s ipv4 %s netmask %s", name,
		     address, mask) >= (int)sizeof(operands))
		return 1;
	return backend("STATIC", operands, 0);
}

static int
apply_interface(const char *name, unsigned timeout)
{
	char key[96], configuration[256], copy[256], *tokens[8];
	char *token;
	int count = 0;
	if (send_up(name) != 0)
		return 1;
	printf("net: %s up\n", name);
	if (snprintf(key, sizeof(key), "net_%s", name) >= (int)sizeof(key))
		return 1;
	if (rcconf_get(ZEDBSD_RC_CONF, key, configuration,
		       sizeof(configuration)) != 0) {
		if (errno == ENOENT)
			return 0;
		fprintf(stderr, "net: invalid %s\n", key);
		return 1;
	}
	strcpy(copy, configuration);
	for (token = strtok(copy, " \t"); token != NULL;
	     token = strtok(NULL, " \t")) {
		if (count == (int)(sizeof(tokens) / sizeof(tokens[0]))) {
			fprintf(stderr, "net: too many fields in %s\n", key);
			return 1;
		}
		tokens[count++] = token;
	}
	if (count == 1 && strcmp(tokens[0], "dhcp") == 0) {
		printf("net: %s dhcp timeout=%u\n", name, timeout);
		return send_dhcp(name, timeout);
	}
	if (count == 5 && strcmp(tokens[0], "static") == 0 &&
	    strcmp(tokens[1], "ipv4") == 0 &&
	    strcmp(tokens[3], "netmask") == 0) {
		printf("net: %s static ipv4\n", name);
		return send_static(name, tokens[2], tokens[4]);
	}
	fprintf(stderr, "net: unsupported or invalid %s\n", key);
	return 1;
}

static int
boot(void)
{
	char automatic[512], copy[512], timeout_text[32] = "";
	char *names[NET_INTERFACE_LIMIT], *name;
	unsigned timeout = 10;
	int count = 0, index, failed = 0;

	if (rcconf_get(ZEDBSD_RC_CONF, "net_auto", automatic,
		       sizeof(automatic)) != 0) {
		if (errno == ENOENT)
			return 0;
		fprintf(stderr, "net: invalid net_auto\n");
		return 1;
	}
	if (rcconf_get(ZEDBSD_RC_CONF, "net_dhcptimeout", timeout_text,
		       sizeof(timeout_text)) == 0) {
		if (decimal_timeout(timeout_text, &timeout) != 0) {
			fprintf(stderr, "net: invalid net_dhcptimeout\n");
			return 1;
		}
	} else if (errno != ENOENT) {
		fprintf(stderr, "net: invalid net_dhcptimeout\n");
		return 1;
	}
	strcpy(copy, automatic);
	for (name = strtok(copy, " \t"); name != NULL;
	     name = strtok(NULL, " \t")) {
		if (!interface_name_valid(name) ||
		    count == NET_INTERFACE_LIMIT) {
			fprintf(stderr, "net: invalid interface in net_auto\n");
			return 1;
		}
		for (index = 0; index < count; index++)
			if (strcmp(names[index], name) == 0) {
				fprintf(stderr, "net: duplicate interface %s\n",
					name);
				return 1;
			}
		names[count++] = name;
	}
	for (index = 0; index < count; index++) {
		printf("net: configuring %s\n", names[index]);
		if (apply_interface(names[index], timeout) != 0)
			failed = 1;
	}
	{
		char gateway[64];
		if (rcconf_get(ZEDBSD_RC_CONF, "net_defaultroute", gateway,
			       sizeof(gateway)) == 0 &&
		    *gateway != '\0' &&
		    backend("DEFAULTROUTE", gateway, 0) != 0)
			failed = 1;
	}
	{
		char dns[256];
		if (rcconf_get(ZEDBSD_RC_CONF, "net_dns", dns, sizeof(dns)) ==
			0 &&
		    *dns != '\0' && backend("DNS", dns, 0) != 0)
			failed = 1;
	}
	return failed;
}

static int
usage(void)
{
	fprintf(
	    stderr,
	    "usage: net boot|show [interface]|up interface|down interface|"
	    "dhcp interface --timeout=seconds|static interface ipv4 address "
	    "netmask mask|defaultroute gateway|dns address...\n");
	return 2;
}

int
main(int argc, char **argv)
{
	char operands[NETWORKD_REQUEST_MAX];
	int length;
	unsigned timeout;

	if (argc == 2 && strcmp(argv[1], "boot") == 0)
		return boot();
	if (argc >= 2 && strcmp(argv[1], "show") == 0 && argc <= 3)
		return backend("SHOW", argc == 3 ? argv[2] : NULL, 1);
	if (argc == 3 &&
	    (strcmp(argv[1], "up") == 0 || strcmp(argv[1], "down") == 0) &&
	    interface_name_valid(argv[2]))
		return backend(strcmp(argv[1], "up") == 0 ? "UP" : "DOWN",
			       argv[2], 0);
	if (argc == 4 && strcmp(argv[1], "dhcp") == 0 &&
	    interface_name_valid(argv[2]) &&
	    strncmp(argv[3], "--timeout=", 10) == 0 &&
	    decimal_timeout(argv[3] + 10, &timeout) == 0) {
		length = snprintf(operands, sizeof(operands), "%s %u", argv[2],
				  timeout);
		return length < 0 || (size_t)length >= sizeof(operands)
			   ? 1
			   : backend("DHCP", operands, 0);
	}
	if (argc == 7 && strcmp(argv[1], "static") == 0 &&
	    interface_name_valid(argv[2]) && strcmp(argv[3], "ipv4") == 0 &&
	    strcmp(argv[5], "netmask") == 0) {
		length = snprintf(operands, sizeof(operands),
				  "%s ipv4 %s netmask %s", argv[2], argv[4],
				  argv[6]);
		return length < 0 || (size_t)length >= sizeof(operands)
			   ? 1
			   : backend("STATIC", operands, 0);
	}
	if (argc == 3 && strcmp(argv[1], "defaultroute") == 0)
		return backend("DEFAULTROUTE", argv[2], 0);
	if (argc >= 3 && argc <= NET_DNS_LIMIT + 2 &&
	    strcmp(argv[1], "dns") == 0) {
		int index;
		size_t used = 0;
		for (index = 2; index < argc; index++) {
			struct in_addr parsed;
			if (inet_aton(argv[index], &parsed) == 0)
				return usage();
			length =
			    snprintf(operands + used, sizeof(operands) - used,
				     "%s%s", used == 0 ? "" : " ", argv[index]);
			if (length < 0 ||
			    (size_t)length >= sizeof(operands) - used)
				return 1;
			used += (size_t)length;
		}
		return backend("DNS", operands, 0);
	}
	return usage();
}
