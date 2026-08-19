/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/libc/resolver-internal.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
ptr_name(struct in_addr address, char output[64])
{
	uint32_t value = ntohl(address.s_addr);
	return snprintf(output, 64, "%u.%u.%u.%u.in-addr.arpa",
	    value & 255U, value >> 8 & 255U, value >> 16 & 255U,
	    value >> 24 & 255U);
}

static void
print_result(const char *query, const struct resolver_result *result)
{
	char server[16], address[16]; unsigned i;
	inet_ntop(AF_INET, &result->server, server, sizeof(server));
	printf("Server: %s#%u\nName: %s\n", server, result->port, query);
	for (i = 0; i < result->cname_count; i++) printf("Canonical name: %s\n", result->cname_chain[i]);
	for (i = 0; i < result->address_count; i++) { inet_ntop(AF_INET, &result->addresses[i], address, sizeof(address)); printf("Address: %s\n", address); }
	if (result->ptr_name[0] != '\0') printf("Name: %s\n", result->ptr_name);
	printf("TTL: %u\n", result->ttl);
}

static int usage(void) { puts("usage: nslookup [-p port] name [server]"); return 2; }

int
main(int argc, char **argv)
{
	struct resolver_result result;
	struct resolver_config config;
	struct in_addr server, numeric;
	char query[254], *end;
	unsigned long port = 53;
	unsigned arg = 1, index;
	uint16_t type;
	int error = EAI_AGAIN;
	if (argc > 2 && strcmp(argv[1], "-p") == 0) { port = strtoul(argv[2], &end, 10); if (*end != '\0' || port == 0 || port > 65535U) return usage(); arg = 3; }
	if (arg >= (unsigned)argc || arg + 2U < (unsigned)argc) return usage();
	if (inet_aton(argv[arg], &numeric)) { ptr_name(numeric, query); type = DNS_TYPE_PTR; } else { if (strlen(argv[arg]) >= sizeof(query)) return usage(); strcpy(query, argv[arg]); type = DNS_TYPE_A; }
	if (arg + 1U < (unsigned)argc) {
		if (!inet_aton(argv[arg + 1], &server)) return usage();
		error = resolver_query_server(query, type, &server, (uint16_t)port, &result);
	} else if (port == 53U) error = resolver_query(query, type, &result);
	else if ((error = resolver_load_config(&config)) == 0) for (index = 0; index < config.count; index++) { error = resolver_query_server(query, type, &config.servers[index], (uint16_t)port, &result); if (error == 0 || error == EAI_NONAME) break; }
	if (error != 0) { printf("nslookup: %s: %s\n", argv[arg], gai_strerror(error)); return 1; }
	print_result(argv[arg], &result); return 0;
}
