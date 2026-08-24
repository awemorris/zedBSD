/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_RESOLVER_INTERNAL_H
#define ZEDBSD_RESOLVER_INTERNAL_H

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>

#define DNS_TYPE_A 1U
#define DNS_TYPE_CNAME 5U
#define DNS_TYPE_PTR 12U
#define DNS_MAX_ADDRESSES 8U
#define DNS_MAX_NAMESERVERS 3U

struct resolver_result {
	struct in_addr addresses[DNS_MAX_ADDRESSES];
	unsigned address_count;
	char canonical[254];
	char ptr_name[254];
	char cname_chain[8][254];
	unsigned cname_count;
	uint32_t ttl;
	struct in_addr server;
	uint16_t port;
};

struct resolver_config {
	struct in_addr servers[DNS_MAX_NAMESERVERS];
	unsigned count;
};

int resolver_dns_build_query(uint8_t *, size_t, uint16_t, const char *,
			     uint16_t, size_t *);
int resolver_dns_parse(const uint8_t *, size_t, uint16_t, const char *,
		       uint16_t, struct resolver_result *, int *);
int resolver_load_config(struct resolver_config *);
int resolver_query_server(const char *, uint16_t, const struct in_addr *,
			  uint16_t, struct resolver_result *);
int resolver_query(const char *, uint16_t, struct resolver_result *);

#endif
