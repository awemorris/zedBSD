/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD nslookup userland command.
 */

#include "userland/base/libc/resolver-internal.h"

#include <arpa/inet.h>
#include <net/if.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int usage(void);
static int ptr_name(struct in_addr address, char output[64]);
static void print_result(const char *query, const struct resolver_result *result);
static int nslookup_ask(const char *query, uint16_t type, const struct resolver_server *server, unsigned long port, struct resolver_result *result);
static int nslookup_server(const char *text, struct resolver_server *server);
static int nslookup_ask_one(const char *query, uint16_t type, const struct resolver_server *server, unsigned long port, struct resolver_result *result);

/*
 * Runs the nslookup command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	struct resolver_result result;
	struct resolver_result result6;
	struct in_addr numeric;
	struct in6_addr numeric6;
	struct resolver_server server;
	const struct resolver_server *given;
	char query[254], *end;
	unsigned long port;
	unsigned arg;
	uint16_t type;
	int error;
	int error6;
	int parsed;

	port = 53;
	arg = 1;
	error = EAI_AGAIN;

	/* Handles the selected command-line operation. */
	if (argc > 2 && strcmp(argv[1], "-p") == 0) {
		port = strtoul(argv[2], &end, 10);

		/* Checks the current endpoint. */
		if (*end != '\0' || port == 0 || port > 65535U) {
			/* Obtains the usage result. */
			function_result = usage();

			/* Returns the computed result. */
			return function_result;
		}
		arg = 3;
	}

	/* Validates the command-line arguments. */
	if (arg >= (unsigned)argc || arg + 2U < (unsigned)argc) {
		/* Obtains the usage result. */
		function_result = usage();

		/* Returns the computed result. */
		return function_result;
	}

	/* An IPv4 or IPv6 address asks for its name (PTR); a name asks for its IPv4 and IPv6 addresses (ws130-p005). */
	type = DNS_TYPE_A;
	if (inet_aton(argv[arg], &numeric)) {
		ptr_name(numeric, query);
		type = DNS_TYPE_PTR;
	} else if (inet_pton(AF_INET6, argv[arg], &numeric6) == 1) {
		(void)resolver_inet6_ptr_name(numeric6.s6_addr, query, sizeof(query));
		type = DNS_TYPE_PTR;
	} else {
		/* Validates the command-line arguments. */
		if (strlen(argv[arg]) >= sizeof(query)) {
			/* Obtains the usage result. */
			function_result = usage();

			/* Returns the computed result. */
			return function_result;
		}
		strcpy(query, argv[arg]);
	}

	/* The server named after the query (IPv4, or IPv6 with %zone, ws177-p044), or resolv.conf's. */
	given = NULL;
	if (arg + 1U < (unsigned)argc) {
		/* Validates the command-line arguments. */
		parsed = nslookup_server(argv[arg + 1], &server);
		if (!parsed) {
			/* Obtains the usage result. */
			function_result = usage();

			/* Returns the computed result. */
			return function_result;
		}
		given = &server;
	}

	/* The question; for a name its AAAA records too, kept with the A ones. */
	error = nslookup_ask(query, type, given, port, &result);
	if (type == DNS_TYPE_A) {
		error6 = nslookup_ask(query, DNS_TYPE_AAAA, given, port, &result6);
		if (error6 == 0 && error != 0) {
			result = result6;
			result.address_count = 0;
			error = 0;
		} else if (error6 == 0) {
			memcpy(result.addresses6, result6.addresses6, sizeof(result.addresses6));
			result.address6_count = result6.address6_count;
		}
	}

	/* Handles an operation failure. */
	if (error != 0) {
		printf("nslookup: %s: %s\n", argv[arg], gai_strerror(error));

		/* Reports operation failure. */
		return 1;
	}
	print_result(argv[arg], &result);

	/* Reports successful completion. */
	return 0;
}

/* Supports the usage operation. */
static int
usage(
	void)
{
	puts("usage: nslookup [-p port] name [server (IPv4, or IPv6 with %zone)]");

	/* Reports operation failure. */
	return 2;
}

/* Supports the ptr name operation. */
static int
ptr_name(
	struct in_addr address,
	char output[64])
{
	int function_result;
	uint32_t value;

	value = ntohl(address.s_addr);

	/* Obtains the snprintf result. */
	function_result = snprintf(output, 64, "%u.%u.%u.%u.in-addr.arpa", value & 255U,
			value >> 8 & 255U, value >> 16 & 255U,
			value >> 24 & 255U);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the print result operation. */
static void
print_result(
	const char *query,
	const struct resolver_result *result)
{
	char server[INET6_ADDRSTRLEN + IF_NAMESIZE + 2], address[INET6_ADDRSTRLEN];
	char zone[IF_NAMESIZE];
	const char *named;
	size_t length;
	unsigned i;

	/* The server that answered: IPv4, or IPv6 with its interface (ws177-p044). */
	if (result->server_family == AF_INET6) {
		inet_ntop(AF_INET6, &result->server6, server, sizeof(server));
		if (result->server6_scope != 0U) {
			length = strlen(server);
			named = if_indextoname(result->server6_scope, zone);
			if (named != NULL)
				snprintf(server + length, sizeof(server) - length, "%%%s", zone);
			else
				snprintf(server + length, sizeof(server) - length, "%%%u", (unsigned)result->server6_scope);
		}
	} else {
		inet_ntop(AF_INET, &result->server, server, sizeof(server));
	}
	printf("Server: %s#%u\nName: %s\n", server, result->port, query);

	/* Process each remaining element. */
	for (i = 0; i < result->cname_count; i++)
		printf("Canonical name: %s\n", result->cname_chain[i]);

	/* Process each remaining element. */
	for (i = 0; i < result->address_count; i++) {
		inet_ntop(AF_INET, &result->addresses[i], address,
			  sizeof(address));
		printf("Address: %s\n", address);
	}

	/* The IPv6 ones (ws130-p005). */
	for (i = 0; i < result->address6_count; i++) {
		inet_ntop(AF_INET6, &result->addresses6[i], address, sizeof(address));
		printf("Address: %s\n", address);
	}

	/* Checks the operation result. */
	if (result->ptr_name[0] != '\0')
		printf("Name: %s\n", result->ptr_name);
	printf("TTL: %u\n", result->ttl);
}

/*
 * Asks one question: of the server given, of resolv.conf's servers in
 * their order (port 53), or of resolv.conf's servers on another port.
 */
static int
nslookup_ask(
	const char *query,
	uint16_t type,
	const struct resolver_server *server,
	unsigned long port,
	struct resolver_result *result)
{
	struct resolver_config config;
	unsigned index;
	int error;

	/* The server given. */
	if (server != NULL) {
		error = nslookup_ask_one(query, type, server, port, result);
		return error;
	}

	/* resolv.conf's servers. */
	if (port == 53U) {
		error = resolver_query(query, type, result);
		return error;
	}

	/* Its servers on another port, IPv4 and IPv6 (ws177-p044). */
	error = resolver_load_config(&config);
	if (error != 0)
		return error;
	for (index = 0; index < config.list_count; index++) {
		error = nslookup_ask_one(query, type, &config.list[index], port, result);
		if (error == 0 || error == EAI_NONAME)
			break;
	}

	/* The last answer. */
	return error;
}

/* Asks one server, IPv4 or IPv6. */
static int
nslookup_ask_one(
	const char *query,
	uint16_t type,
	const struct resolver_server *server,
	unsigned long port,
	struct resolver_result *result)
{
	int error;

	/* An IPv6 server, with its interface, or an IPv4 one. */
	if (server->family == AF_INET6) {
		error = resolver_query_server6(query, type, &server->address6, server->scope, (uint16_t)port, result);
	} else {
		error = resolver_query_server(query, type, &server->address, (uint16_t)port, result);
	}

	/* Reports why it was not answered. */
	if (error != 0)
		return error;

	/* Succeeded: the answer. */
	return 0;
}

/* Reads a server named on the command line: an IPv4 address, or an IPv6 one with "%zone"; returns 1 when it is one. */
static int
nslookup_server(
	const char *text,
	struct resolver_server *server)
{
	char copy[INET6_ADDRSTRLEN + IF_NAMESIZE + 2];
	size_t length;
	int parsed;

	/* An IPv4 address. */
	memset(server, 0, sizeof(*server));
	parsed = inet_aton(text, &server->address);
	if (parsed) {
		server->family = AF_INET;
		return 1;
	}

	/* An IPv6 one, read from a copy (the zone is cut off in place). */
	length = strlen(text);
	if (length >= sizeof(copy))
		return 0;
	strcpy(copy, text);
	parsed = resolver_parse_server6(copy, server);
	if (!parsed)
		return 0;

	/* Succeeded: an IPv6 server. */
	return 1;
}
