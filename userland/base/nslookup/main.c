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
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int usage(void);
static int ptr_name(struct in_addr address, char output[64]);
static void print_result(const char *query, const struct resolver_result *result);

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
	struct resolver_config config;
	struct in_addr server, numeric;
	char query[254], *end;
	unsigned long port;
	unsigned arg, index;
	uint16_t type;
	int error;

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

	/* Validates the command-line arguments. */
	if (inet_aton(argv[arg], &numeric)) {
		ptr_name(numeric, query);
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
		type = DNS_TYPE_A;
	}

	/* Validates the command-line arguments. */
	if (arg + 1U < (unsigned)argc) {
		/* Validates the command-line arguments. */
		if (!inet_aton(argv[arg + 1], &server)) {
			/* Obtains the usage result. */
			function_result = usage();

			/* Returns the computed result. */
			return function_result;
		}
		error = resolver_query_server(query, type, &server,
					      (uint16_t)port, &result);
	} else if (port == 53U)
		error = resolver_query(query, type, &result);
	else if ((error = resolver_load_config(&config)) == 0)

		/* Process each remaining element. */
		for (index = 0; index < config.count; index++) {
			error = resolver_query_server(query, type,
						      &config.servers[index],
						      (uint16_t)port, &result);

			/* Handles an operation failure. */
			if (error == 0 || error == EAI_NONAME)
				break;
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
	puts("usage: nslookup [-p port] name [server]");

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
	char server[16], address[16];
	unsigned i;

	inet_ntop(AF_INET, &result->server, server, sizeof(server));
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

	/* Checks the operation result. */
	if (result->ptr_name[0] != '\0')
		printf("Name: %s\n", result->ptr_name);
	printf("TTL: %u\n", result->ttl);
}
