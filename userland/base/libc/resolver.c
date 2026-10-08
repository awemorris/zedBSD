/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD C library resolver support.
 */

#include "userland/base/libc/resolver-internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static uint32_t resolver_counter;
static pthread_mutex_t resolver_counter_lock = PTHREAD_MUTEX_INITIALIZER;

static int resolver_query_server_depth(const char *name, uint16_t type, const struct sockaddr *server, socklen_t server_length, struct resolver_result *result, unsigned depth);
static int resolver_same_peer(const struct sockaddr *server, const struct sockaddr_storage *source);
static uint16_t query_id(const char *name);
static int tcp_query(const struct sockaddr *server, socklen_t server_length, const uint8_t *query, size_t query_length, uint16_t id, const char *name, uint16_t type, struct resolver_result *result);
static int write_all_socket(int descriptor, const uint8_t *buffer, size_t length);
static int read_exact_socket(int descriptor, uint8_t *buffer, size_t length);
static int parse_service(const char *service, const char *protocol, uint16_t *port);
static int make_ptr_name(struct in_addr address, char *output, size_t capacity);

/* The flags getaddrinfo and getnameinfo take (ws130-p004). */
#define GAI_FLAGS	(AI_PASSIVE | AI_CANONNAME | AI_NUMERICHOST | AI_NUMERICSERV | AI_ALL | AI_ADDRCONFIG | AI_V4MAPPED)
#define GNI_FLAGS	(NI_NUMERICHOST | NI_NUMERICSERV | NI_NAMEREQD | NI_NOFQDN | NI_DGRAM | NI_NUMERICSCOPE)

/* How many addresses getaddrinfo gives at most: the A and the AAAA records. */
#define GAI_ADDRESSES	(2U * DNS_MAX_ADDRESSES)

/* One address getaddrinfo found: IPv4, or IPv6 with its scope. */
struct gai_address {
	int family;
	struct in_addr address;
	struct in6_addr address6;
	uint32_t scope;
};

/* The addresses getaddrinfo found, in their order, and the canonical name. */
struct gai_list {
	struct gai_address items[GAI_ADDRESSES];
	unsigned count;
	char canonical[254];
};

static void gai_add4(struct gai_list *list, struct in_addr address, int mapped);
static void gai_add6(struct gai_list *list, const struct in6_addr *address, uint32_t scope);
static void gai_unnamed(struct gai_list *list, int family, int flags);
static int gai_numeric(const char *node, int family, int flags, struct gai_list *list);
static int gai_hosts(const char *node, int family, int flags, struct gai_list *list);
static int gai_lookup(const char *node, int family, int flags, struct gai_list *list);
static int gai_source(const struct gai_address *item, struct sockaddr_storage *source);
static void gai_reachable_only(struct gai_list *list);
static int gai_configured(int *have4, int *have6);
static int gai_service_name(uint16_t port, int flags, char *service, socklen_t service_length);
static void gai_order(struct gai_list *list);
static int gai_build(const struct gai_list *list, uint16_t port, int socktype, int protocol, int flags, struct addrinfo **output);
static int gni_numeric6(const struct sockaddr_in6 *inet6, int flags, char *host, socklen_t host_length);

/*
 * Implements the resolver load config operation.
 */
int
resolver_load_config(
	struct resolver_config *config)
{
	char *text, *end;
	FILE *file;
	char line[128];

	/* Handles the config availability. */
	if (config == NULL)
		return EAI_FAIL;
	memset(config, 0, sizeof(*config));
	file = fopen("/etc/resolv.conf", "r");

	/* Handles the file availability. */
	if (file == NULL)
		return EAI_AGAIN;

	/* Process input until it is exhausted. */
	while (config->list_count < DNS_MAX_NAMESERVERS &&
	       fgets(line, sizeof(line), file) != NULL) {
		/* Continue while the operation condition remains true. */
		text = line;
		while (*text == ' ' || *text == '\t')
			text++;

		/* Validates the current text. */
		if (*text == '#' || *text == '\n' || *text == '\0')
			continue;

		/* Selects the matching prefix. */
		if (strncmp(text, "nameserver", 10U) != 0 ||
		    (text[10] != ' ' && text[10] != '\t'))
			continue;
		text += 10;

		/* Continue while the operation condition remains true. */
		while (*text == ' ' || *text == '\t')
			text++;

		/* Continue while the operation condition remains true. */
		end = text;
		while (*end != '\0' && *end != '\n' && *end != '\r' &&
		       *end != ' ' && *end != '\t' && *end != '#')
			end++;
		*end = '\0';

		/* An IPv4 server, kept in both lists. */
		if (inet_aton(text, &config->servers[config->count])) {
			config->list[config->list_count].family = AF_INET;
			config->list[config->list_count].address = config->servers[config->count];
			config->list_count++;
			config->count++;
			continue;
		}

		/* An IPv6 one, with the interface of a link-local address (ws130-p004). */
		if (resolver_parse_server6(text, &config->list[config->list_count]))
			config->list_count++;
	}
	fclose(file);

	/* Returns the computed result. */
	return config->list_count != 0U ? 0 : EAI_AGAIN;
}

/*
 * Implements the resolver query server operation.
 */
int
resolver_query_server(
	const char *name,
	uint16_t type,
	const struct in_addr *server_address,
	uint16_t port,
	struct resolver_result *result)
{
	struct sockaddr_in server;
	int function_result;

	/* The server's address. */
	if (server_address == NULL)
		return EAI_FAIL;
	memset(&server, 0, sizeof(server));
	server.sin_family = AF_INET;
	server.sin_port = htons(port);
	server.sin_addr = *server_address;

	/* Obtains the resolver query server depth result. */
	function_result = resolver_query_server_depth(name, type, (const struct sockaddr *)&server, sizeof(server), result, 0);

	/* Succeeded or not, the server that answered. */
	if (function_result == 0) {
		result->server = *server_address;
		result->port = port;
		result->server_family = AF_INET;
	}
	return function_result;
}

/*
 * Implements the resolver query operation.
 */
int
resolver_query(
	const char *name,
	uint16_t type,
	struct resolver_result *result)
{
	struct resolver_config config;
	unsigned index;
	int error;

	error = resolver_load_config(&config);

	/* Handles an operation failure. */
	if (error != 0)
		return error;

	/* Each server in resolv.conf's order, IPv4 or IPv6 (ws130-p004). */
	for (index = 0; index < config.list_count; index++) {
		if (config.list[index].family == AF_INET6) {
			error = resolver_query_server6(name, type, &config.list[index].address6, config.list[index].scope, 53U, result);
		} else {
			error = resolver_query_server(name, type, &config.list[index].address, 53U, result);
		}

		/* An answer, or the name known not to be there, ends the search. */
		if (error == 0 || error == EAI_NONAME)
			return error;
	}

	/* Returns the computed result. */
	return error;
}

/*
 * Finds the addresses of a node and the port of a service (POSIX): IPv4
 * (A) and IPv6 (AAAA) ones (ws130-p004), a numeric address of either
 * family (an IPv6 one with "%zone"), IPv4 ones as v4-mapped IPv6 ones for
 * AF_INET6 with AI_V4MAPPED, only the families the host can reach with
 * AI_ADDRCONFIG, and IPv6 before IPv4 only when the host has a source that
 * reaches the IPv6 destination (RFC 6724, resolver_inet6_preferred).
 */
int
getaddrinfo(
	const char *node,
	const char *service,
	const struct addrinfo *hints,
	struct addrinfo **output)
{
	struct gai_list list;
	uint16_t port;
	int family;
	int socktype;
	int protocol;
	int flags;
	int error;

	/* The hints. */
	if (output == NULL)
		return EAI_FAIL;
	*output = NULL;
	family = AF_UNSPEC;
	socktype = 0;
	protocol = 0;
	flags = 0;
	if (hints != NULL) {
		family = hints->ai_family;
		socktype = hints->ai_socktype;
		protocol = hints->ai_protocol;
		flags = hints->ai_flags;
	}

	/* The flags, the family and the socket type known. */
	if ((flags & ~GAI_FLAGS) != 0)
		return EAI_BADFLAGS;
	if (family != AF_UNSPEC && family != AF_INET && family != AF_INET6)
		return EAI_FAMILY;
	if (socktype != 0 &&
	    socktype != SOCK_DGRAM &&
	    socktype != SOCK_STREAM &&
	    socktype != SOCK_RAW)
		return EAI_SOCKTYPE;

	/* The service's port. */
	error = parse_service(service, socktype == SOCK_DGRAM ? "udp" : "tcp", &port);
	if (error != 0)
		return error;

	/* The addresses: none named, a numeric one, or the names' in the DNS. */
	memset(&list, 0, sizeof(list));
	if (node == NULL) {
		gai_unnamed(&list, family, flags);
	} else {
		error = gai_numeric(node, family, flags, &list);
		if (error == EAI_NONAME && (flags & AI_NUMERICHOST) == 0)
			error = gai_hosts(node, family, flags, &list);
		if (error == EAI_NONAME && (flags & AI_NUMERICHOST) == 0)
			error = gai_lookup(node, family, flags, &list);
		if (error != 0)
			return error;
	}

	/* The families the host reaches, and IPv6 before IPv4 when the host reaches it. */
	if ((flags & AI_ADDRCONFIG) != 0 && node != NULL)
		gai_reachable_only(&list);
	if (list.count == 0U)
		return EAI_NONAME;
	if (node != NULL)
		gai_order(&list);

	/* Succeeded: the list. */
	error = gai_build(&list, port, socktype, protocol, flags, output);
	return error;
}

/*
 * Implements the freeaddrinfo operation.
 */
void
freeaddrinfo(
	struct addrinfo *info)
{
	struct addrinfo *next;

	/* Continue while the operation condition remains true. */
	while (info != NULL) {
		next = info->ai_next;
		free(info->ai_addr);
		free(info->ai_canonname);
		free(info);
		info = next;
	}
}

/*
 * Implements the gai strerror operation.
 */
const char *
gai_strerror(
	int error)
{
	/* Dispatch the selected operation case. */
	switch (error) {
	case 0:
		/* Returns the computed result. */
		return "success";
	case EAI_AGAIN:
		/* Returns the computed result. */
		return "temporary failure in name resolution";
	case EAI_BADFLAGS:
		/* Returns the computed result. */
		return "invalid resolver flags";
	case EAI_FAIL:
		/* Returns the computed result. */
		return "name server failure";
	case EAI_FAMILY:
		/* Returns the computed result. */
		return "unsupported address family";
	case EAI_MEMORY:
		/* Returns the computed result. */
		return "out of memory";
	case EAI_NONAME:
		/* Returns the computed result. */
		return "name or service not known";
	case EAI_SERVICE:
		/* Returns the computed result. */
		return "unsupported service";
	case EAI_SOCKTYPE:
		/* Returns the computed result. */
		return "unsupported socket type";
	case EAI_OVERFLOW:
		/* Returns the computed result. */
		return "result buffer too small";
	case EAI_SYSTEM:
		/* Returns the computed result. */
		return "system error";
	default:
		/* Returns the computed result. */
		return "resolver error";
	}
}

/*
 * Names an address and its port (POSIX): an IPv4 or an IPv6 address
 * (ws130-p004; a link-local one with "%interface", or "%index" with
 * NI_NUMERICSCOPE), by its PTR record unless NI_NUMERICHOST.
 */
int
getnameinfo(
	const struct sockaddr *address,
	socklen_t length,
	char *host,
	socklen_t host_length,
	char *service,
	socklen_t service_length,
	int flags)
{
	const struct sockaddr_in *inet;
	const struct sockaddr_in6 *inet6;
	struct resolver_result result;
	const char *written;
	char buffer[254];
	uint16_t port;
	int needed;
	int error;

	/* The flags known, and an address of a family known. */
	if ((flags & ~GNI_FLAGS) != 0)
		return EAI_BADFLAGS;
	if (address == NULL)
		return EAI_FAMILY;
	inet = (const struct sockaddr_in *)address;
	inet6 = (const struct sockaddr_in6 *)address;
	if (address->sa_family == AF_INET && length >= sizeof(*inet)) {
		port = inet->sin_port;
	} else if (address->sa_family == AF_INET6 && length >= sizeof(*inet6)) {
		port = inet6->sin6_port;
	} else {
		return EAI_FAMILY;
	}

	/* The service: its name in /etc/services (udp's with NI_DGRAM, ws177-p044), else the port's number. */
	if (service != NULL && service_length != 0U) {
		error = gai_service_name(ntohs(port), flags, service, service_length);
		if (error != 0)
			return error;
	}

	/* No host asked for. */
	if (host == NULL || host_length == 0U)
		return 0;

	/* The name of its PTR record. */
	if ((flags & NI_NUMERICHOST) == 0) {
		if (address->sa_family == AF_INET6)
			error = resolver_inet6_ptr_name(inet6->sin6_addr.s6_addr, buffer, sizeof(buffer));
		else
			error = make_ptr_name(inet->sin_addr, buffer, sizeof(buffer));
		if (error == 0)
			error = resolver_query(buffer, DNS_TYPE_PTR, &result);
		if (error == 0) {
			/* Its first label alone with NI_NOFQDN (ws177-p044). */
			if ((flags & NI_NOFQDN) != 0)
				resolver_short_name(result.ptr_name);
			needed = (int)strlen(result.ptr_name);
			if ((socklen_t)needed + 1U > host_length)
				return EAI_OVERFLOW;
			strcpy(host, result.ptr_name);
			return 0;
		}

		/* Without a name, the number unless a name was required. */
		if ((flags & NI_NAMEREQD) != 0)
			return error;
	}

	/* Succeeded: the number. */
	if (address->sa_family == AF_INET6) {
		error = gni_numeric6(inet6, flags, host, host_length);
		return error;
	}
	written = inet_ntop(AF_INET, &inet->sin_addr, buffer, sizeof(buffer));
	if (written == NULL)
		return EAI_SYSTEM;
	needed = (int)strlen(buffer);
	if ((socklen_t)needed + 1U > host_length)
		return EAI_OVERFLOW;
	strcpy(host, buffer);
	return 0;
}

/*
 * Asks one server (an IPv4 or an IPv6 address, ws130-p004) over UDP, then
 * TCP when the answer was cut, and follows a CNAME for an address question
 * (A or AAAA) up to eight deep.
 */
static int
resolver_query_server_depth(
	const char *name,
	uint16_t type,
	const struct sockaddr *server,
	socklen_t server_length,
	struct resolver_result *result,
	unsigned depth)
{
	struct resolver_result target;
	struct sockaddr_storage source;
	struct timeval timeout;
	char alias[254];
	uint8_t query[512];
	uint8_t response[512];
	uint32_t cname_ttl;
	socklen_t source_length;
	size_t query_length;
	ssize_t count;
	uint16_t id;
	int attempt;
	int descriptor;
	int error;
	int truncated;
	int same;
	int address_question;

	/* The question. */
	if (name == NULL || server == NULL || result == NULL)
		return EAI_FAIL;
	memset(result, 0, sizeof(*result));
	id = query_id(name);
	error = resolver_dns_build_query(query, sizeof(query), id, name, type, &query_length);
	if (error != 0)
		return error;

	/* Two tries over UDP. */
	for (attempt = 0; attempt < 2; attempt++) {
		descriptor = socket(server->sa_family, SOCK_DGRAM, IPPROTO_UDP);
		if (descriptor < 0)
			return EAI_SYSTEM;
		timeout.tv_sec = 2;
		timeout.tv_usec = 0;
		(void)setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

		/* The question out, an answer from the server back. */
		count = sendto(descriptor, query, query_length, 0, server, server_length);
		if (count < 0) {
			close(descriptor);
			continue;
		}
		source_length = sizeof(source);
		memset(&source, 0, sizeof(source));
		count = recvfrom(descriptor, response, sizeof(response), 0, (struct sockaddr *)&source, &source_length);
		same = 0;
		if (count >= 0)
			same = resolver_same_peer(server, &source);
		if (!same) {
			close(descriptor);
			continue;
		}

		/* The answer, over TCP again when it was cut. */
		error = resolver_dns_parse(response, (size_t)count, id, name, type, result, &truncated);
		close(descriptor);
		if (truncated)
			error = tcp_query(server, server_length, query, query_length, id, name, type, result);

		/* An address question answered with a CNAME only: the alias asked in turn. */
		address_question = type == DNS_TYPE_A || type == DNS_TYPE_AAAA;
		if (error != EAI_NONAME || !address_question)
			return error;
		if (result->canonical[0] == '\0' || depth >= 8U)
			return error;
		cname_ttl = result->ttl;
		strncpy(alias, result->canonical, sizeof(alias) - 1U);
		alias[sizeof(alias) - 1U] = '\0';
		error = resolver_query_server_depth(alias, type, server, server_length, &target, depth + 1U);
		if (error != 0)
			return error;

		/* The alias's answer, the alias first in its chain, the shorter time to live. */
		*result = target;
		if (result->cname_count < 8U) {
			memmove(result->cname_chain + 1, result->cname_chain, result->cname_count * sizeof(result->cname_chain[0]));
			strncpy(result->cname_chain[0], alias, 253U);
			result->cname_count++;
		}
		if (cname_ttl != 0 && (result->ttl == 0 || cname_ttl < result->ttl))
			result->ttl = cname_ttl;

		/* Succeeded: answered through the alias. */
		return 0;
	}

	/* No answer. */
	return EAI_AGAIN;
}

/* Asks an IPv6 server (the interface of a link-local one in scope) (ws130-p004). */
int
resolver_query_server6(
	const char *name,
	uint16_t type,
	const struct in6_addr *address,
	uint32_t scope,
	uint16_t port,
	struct resolver_result *result)
{
	struct sockaddr_in6 server;
	int error;

	/* The server's address. */
	memset(&server, 0, sizeof(server));
	server.sin6_family = AF_INET6;
	server.sin6_port = htons(port);
	server.sin6_addr = *address;
	server.sin6_scope_id = scope;

	/* The question. */
	error = resolver_query_server_depth(name, type, (const struct sockaddr *)&server, sizeof(server), result, 0);
	if (error != 0)
		return error;

	/* Succeeded: the server that answered (ws177-p044). */
	result->server_family = AF_INET6;
	result->server6 = *address;
	result->server6_scope = scope;
	result->port = port;
	return 0;
}

/* Tells whether an answer came from the server asked: its family, address and port (ws130-p004). */
static int
resolver_same_peer(
	const struct sockaddr *server,
	const struct sockaddr_storage *source)
{
	const struct sockaddr_in *server4;
	const struct sockaddr_in *source4;
	const struct sockaddr_in6 *server6;
	const struct sockaddr_in6 *source6;
	int match;

	/* The same family. */
	if (source->ss_family != server->sa_family)
		return 0;

	/* IPv4: the address and the port. */
	if (server->sa_family == AF_INET) {
		server4 = (const struct sockaddr_in *)server;
		source4 = (const struct sockaddr_in *)source;
		if (source4->sin_addr.s_addr != server4->sin_addr.s_addr)
			return 0;
		return source4->sin_port == server4->sin_port;
	}

	/* IPv6: the address and the port. */
	server6 = (const struct sockaddr_in6 *)server;
	source6 = (const struct sockaddr_in6 *)source;
	match = memcmp(&source6->sin6_addr, &server6->sin6_addr, sizeof(server6->sin6_addr));
	if (match != 0)
		return 0;
	return source6->sin6_port == server6->sin6_port;
}

/*
 * Reads an IPv6 name server of resolv.conf, "fe80::1%ue0" (an interface's
 * name or number after the address of a link-local one) (ws130-p004).
 * Returns 1 when it is one.
 */
int
resolver_parse_server6(
	char *text,
	struct resolver_server *server)
{
	char *zone;
	char *end;
	unsigned long number;
	int ok;

	/* The zone after the address. */
	zone = strchr(text, '%');
	if (zone != NULL)
		*zone++ = '\0';

	/* The address. */
	memset(server, 0, sizeof(*server));
	ok = inet_pton(AF_INET6, text, &server->address6);
	if (!ok)
		return 0;
	server->family = AF_INET6;

	/* The zone: a number, or an interface's name. */
	if (zone == NULL)
		return 1;
	number = strtoul(zone, &end, 10);
	if (*zone != '\0' && *end == '\0') {
		server->scope = (uint32_t)number;
		return 1;
	}
	server->scope = if_nametoindex(zone);

	/* Succeeded when the interface is there. */
	return server->scope != 0U;
}

/* Adds an IPv4 address to the list, as a v4-mapped IPv6 one when mapped; a full list drops it. */
static void
gai_add4(
	struct gai_list *list,
	struct in_addr address,
	int mapped)
{
	struct gai_address *item;

	/* A full list. */
	if (list->count == GAI_ADDRESSES)
		return;

	/* As it is. */
	item = &list->items[list->count++];
	memset(item, 0, sizeof(*item));
	if (!mapped) {
		item->family = AF_INET;
		item->address = address;
		return;
	}

	/* Succeeded: as ::ffff:a.b.c.d. */
	item->family = AF_INET6;
	item->address6.s6_addr[10] = 0xff;
	item->address6.s6_addr[11] = 0xff;
	memcpy(&item->address6.s6_addr[12], &address.s_addr, 4U);
}

/* Adds an IPv6 address with its scope to the list; a full list drops it. */
static void
gai_add6(
	struct gai_list *list,
	const struct in6_addr *address,
	uint32_t scope)
{
	struct gai_address *item;

	/* A full list. */
	if (list->count == GAI_ADDRESSES)
		return;

	/* Succeeded: added. */
	item = &list->items[list->count++];
	memset(item, 0, sizeof(*item));
	item->family = AF_INET6;
	item->address6 = *address;
	item->scope = scope;
}

/* The addresses of no node: any (AI_PASSIVE) or the loopback, IPv4 first for AF_UNSPEC. */
static void
gai_unnamed(
	struct gai_list *list,
	int family,
	int flags)
{
	struct in_addr address;
	struct in6_addr address6;

	/* IPv4's. */
	address.s_addr = htonl(0x7f000001U);
	if ((flags & AI_PASSIVE) != 0)
		address.s_addr = htonl(INADDR_ANY);
	if (family != AF_INET6)
		gai_add4(list, address, 0);

	/* And IPv6's. */
	address6 = in6addr_loopback;
	if ((flags & AI_PASSIVE) != 0)
		address6 = in6addr_any;
	if (family != AF_INET)
		gai_add6(list, &address6, 0);
}

/*
 * A numeric node: an IPv4 address (a v4-mapped IPv6 one for AF_INET6 with
 * AI_V4MAPPED), or an IPv6 one with "%zone" (an interface's name or
 * number).  Returns 0, EAI_NONAME when the node is not numeric, or
 * EAI_ADDRFAMILY when its family is not the one asked for.
 */
static int
gai_numeric(
	const char *node,
	int family,
	int flags,
	struct gai_list *list)
{
	struct in_addr address;
	struct in6_addr address6;
	char text[INET6_ADDRSTRLEN + IF_NAMESIZE + 2];
	char *zone;
	char *end;
	unsigned long number;
	uint32_t scope;
	size_t length;
	int ok;

	/* An IPv4 address. */
	ok = inet_aton(node, &address);
	if (ok) {
		if (family == AF_INET6 && (flags & AI_V4MAPPED) == 0)
			return EAI_ADDRFAMILY;
		gai_add4(list, address, family == AF_INET6);
		strncpy(list->canonical, node, sizeof(list->canonical) - 1U);
		return 0;
	}

	/* An IPv6 address, its zone apart. */
	length = strlen(node);
	if (length >= sizeof(text))
		return EAI_NONAME;
	memcpy(text, node, length + 1U);
	zone = strchr(text, '%');
	if (zone != NULL)
		*zone++ = '\0';
	ok = inet_pton(AF_INET6, text, &address6);
	if (ok != 1)
		return EAI_NONAME;
	if (family == AF_INET)
		return EAI_ADDRFAMILY;

	/* The zone: a number, or an interface's name. */
	scope = 0;
	if (zone != NULL) {
		number = strtoul(zone, &end, 10);
		scope = (uint32_t)number;
		if (*zone == '\0' || *end != '\0')
			scope = if_nametoindex(zone);
		if (scope == 0U)
			return EAI_NONAME;
	}

	/* Succeeded: the address. */
	gai_add6(list, &address6, scope);
	strncpy(list->canonical, node, sizeof(list->canonical) - 1U);
	return 0;
}

/*
 * Looks a name up in /etc/hosts, before the DNS: every line that names it,
 * IPv6 ones unless AF_INET, IPv4 ones unless AF_INET6 (for AF_INET6 with
 * AI_V4MAPPED as v4-mapped ones, when there is no IPv6 one or with
 * AI_ALL), in the file's order.  The canonical name is the first matching
 * line's.  Returns 0 when one was found, or EAI_NONAME.
 */
static int
gai_hosts(
	const char *node,
	int family,
	int flags,
	struct gai_list *list)
{
	struct resolver_hosts_entry entry;
	struct in_addr address;
	struct in6_addr address6;
	char line[512];
	FILE *file;
	unsigned v4;
	unsigned v6;
	unsigned pass;
	int mapped;
	int named;

	/* The file. */
	file = fopen("/etc/hosts", "r");
	if (file == NULL)
		return EAI_NONAME;

	/* Two passes: IPv6 lines, then IPv4 ones (mapped for AF_INET6 when allowed). */
	v4 = 0;
	v6 = 0;
	mapped = family == AF_INET6;
	for (pass = 0; pass < 2U; pass++) {
		if (pass == 0U && family == AF_INET)
			continue;
		if (pass == 1U && mapped && ((flags & AI_V4MAPPED) == 0 || (v6 != 0U && (flags & AI_ALL) == 0)))
			continue;
		rewind(file);
		while (fgets(line, sizeof(line), file) != NULL) {
			named = resolver_hosts_line(line, node, &entry);
			if (!named || entry.family != (pass == 0U ? AF_INET6 : AF_INET))
				continue;
			if (list->canonical[0] == '\0')
				strncpy(list->canonical, entry.canonical, sizeof(list->canonical) - 1U);
			if (pass == 0U) {
				memcpy(address6.s6_addr, entry.address, sizeof(address6.s6_addr));
				gai_add6(list, &address6, 0);
				v6++;
			} else {
				memcpy(&address.s_addr, entry.address, sizeof(address.s_addr));
				gai_add4(list, address, mapped);
				v4++;
			}
		}
	}
	fclose(file);

	/* Succeeded when the file named it. */
	if (v4 + v6 == 0U)
		return EAI_NONAME;
	return 0;
}

/*
 * Asks the DNS for a name's addresses: A unless AF_INET6, AAAA unless
 * AF_INET, and for AF_INET6 with AI_V4MAPPED the A records as v4-mapped
 * ones when there is no AAAA (or with AI_ALL too).  Returns 0 when one was
 * found, or the error of the last question.
 */
static int
gai_lookup(
	const char *node,
	int family,
	int flags,
	struct gai_list *list)
{
	struct resolver_result result;
	unsigned index;
	int mapped;
	int want4;
	int error4;
	int error6;

	/* The IPv6 addresses. */
	error6 = EAI_NONAME;
	if (family != AF_INET) {
		error6 = resolver_query(node, DNS_TYPE_AAAA, &result);
		if (error6 == 0) {
			for (index = 0; index < result.address6_count; index++)
				gai_add6(list, &result.addresses6[index], 0);
			strncpy(list->canonical, result.canonical, sizeof(list->canonical) - 1U);
		}
	}

	/* The IPv4 ones, as v4-mapped ones for AF_INET6 (AI_V4MAPPED: when there were no IPv6 ones, or AI_ALL). */
	error4 = EAI_NONAME;
	mapped = family == AF_INET6;
	want4 = !mapped;
	if (mapped && (flags & AI_V4MAPPED) != 0)
		want4 = error6 != 0 || (flags & AI_ALL) != 0;
	if (want4) {
		error4 = resolver_query(node, DNS_TYPE_A, &result);
		if (error4 == 0) {
			for (index = 0; index < result.address_count; index++)
				gai_add4(list, result.addresses[index], mapped);
			if (list->canonical[0] == '\0')
				strncpy(list->canonical, result.canonical, sizeof(list->canonical) - 1U);
		}
	}

	/* No address of either. */
	if (list->count == 0U) {
		if (error4 != EAI_NONAME)
			return error4;
		return error6;
	}

	/* Succeeded: the canonical name, the node's own without a CNAME. */
	if (list->canonical[0] == '\0')
		strncpy(list->canonical, node, sizeof(list->canonical) - 1U);
	return 0;
}

/*
 * Finds the source the host would send from to an address (a UDP socket
 * connected to it, no packet sent): 1 with it, 0 when the host has no way
 * there.
 */
static int
gai_source(
	const struct gai_address *item,
	struct sockaddr_storage *source)
{
	struct sockaddr_in destination;
	struct sockaddr_in6 destination6;
	socklen_t length;
	int descriptor;
	int status;

	/* A socket of the address's family. */
	descriptor = socket(item->family, SOCK_DGRAM, IPPROTO_UDP);
	if (descriptor < 0)
		return 0;

	/* Connected to the address (any port: nothing is sent). */
	memset(&destination, 0, sizeof(destination));
	memset(&destination6, 0, sizeof(destination6));
	if (item->family == AF_INET6) {
		destination6.sin6_family = AF_INET6;
		destination6.sin6_port = htons(53U);
		destination6.sin6_addr = item->address6;
		destination6.sin6_scope_id = item->scope;
		status = connect(descriptor, (const struct sockaddr *)&destination6, sizeof(destination6));
	} else {
		destination.sin_family = AF_INET;
		destination.sin_port = htons(53U);
		destination.sin_addr = item->address;
		status = connect(descriptor, (const struct sockaddr *)&destination, sizeof(destination));
	}

	/* The source it chose. */
	length = sizeof(*source);
	memset(source, 0, sizeof(*source));
	if (status == 0)
		status = getsockname(descriptor, (struct sockaddr *)source, &length);
	close(descriptor);

	/* Succeeded when it was connected. */
	return status == 0;
}

/* Keeps only the addresses of the families the host can send to (AI_ADDRCONFIG). */
static void
gai_reachable_only(
	struct gai_list *list)
{
	struct sockaddr_storage source;
	unsigned index;
	unsigned kept;
	int reach4;
	int reach6;
	int tried4;
	int tried6;
	int reach;
	int configured;

	/*
	 * Each family kept when an interface has an address of it that is
	 * not loopback's (RFC 3493, ws177-p044); only when the interfaces
	 * cannot be read, each judged by whether its first address can be
	 * reached.
	 */
	reach4 = 0;
	reach6 = 0;
	tried4 = 0;
	tried6 = 0;
	configured = gai_configured(&reach4, &reach6);
	if (configured == 0) {
		tried4 = 1;
		tried6 = 1;
	}

	/* Each address, its family judged once. */
	kept = 0;
	for (index = 0; index < list->count; index++) {
		if (list->items[index].family == AF_INET6 && !tried6) {
			reach6 = gai_source(&list->items[index], &source);
			tried6 = 1;
		}
		if (list->items[index].family == AF_INET && !tried4) {
			reach4 = gai_source(&list->items[index], &source);
			tried4 = 1;
		}

		/* Kept when its family is reached. */
		reach = reach4;
		if (list->items[index].family == AF_INET6)
			reach = reach6;
		if (!reach)
			continue;
		list->items[kept++] = list->items[index];
	}

	/* The list without the others. */
	list->count = kept;
}

/*
 * Tells which families have an address on an interface that counts for
 * AI_ADDRCONFIG (resolver_usable4, resolver_usable6): every interface
 * (SIOCGIFCONF), its IPv4 address (SIOCGIFADDR) and its IPv6 ones
 * (SIOCGIFADDRS_IN6).  Returns 0, or -1 when the interfaces cannot be read.
 */
static int
gai_configured(
	int *have4,
	int *have6)
{
	struct ifreq names[32];
	struct ifreq request;
	struct ifconf list;
	struct in6_ifaddrs addresses;
	const struct sockaddr_in *inet;
	const struct in6_ifaddr_entry *entry;
	unsigned count;
	unsigned index;
	unsigned at;
	int descriptor;
	int status;
	int settled;
	int usable;

	/* A socket to ask on, and the interfaces' names. */
	*have4 = 0;
	*have6 = 0;
	descriptor = socket(AF_INET, SOCK_DGRAM, 0);
	if (descriptor < 0)
		return -1;
	memset(&list, 0, sizeof(list));
	memset(names, 0, sizeof(names));
	list.ifc_len = (uint32_t)sizeof(names);
	list.ifc_buf = (uint64_t)(uintptr_t)names;
	status = ioctl(descriptor, SIOCGIFCONF, &list);
	if (status != 0) {
		close(descriptor);
		return -1;
	}

	/* How many names came. */
	count = list.ifc_len / (uint32_t)sizeof(names[0]);

	/* Each interface's addresses. */
	for (index = 0; index < count; index++) {
		/* Its IPv4 address. */
		memset(&request, 0, sizeof(request));
		memcpy(request.ifr_name, names[index].ifr_name, sizeof(request.ifr_name));
		status = ioctl(descriptor, SIOCGIFADDR, &request);
		if (status == 0) {
			inet = (const struct sockaddr_in *)(const void *)&request.ifr_addr;
			usable = resolver_usable4(ntohl(inet->sin_addr.s_addr));
			if (usable)
				*have4 = 1;
		}

		/* Its IPv6 ones (none: IPv6 off there). */
		memset(&addresses, 0, sizeof(addresses));
		memcpy(addresses.ifa_name, names[index].ifr_name, sizeof(addresses.ifa_name));
		status = ioctl(descriptor, SIOCGIFADDRS_IN6, &addresses);
		if (status != 0)
			continue;
		for (at = 0; at < addresses.ifa_count && at < IN6_IFADDRS_MAX; at++) {
			entry = &addresses.ifa_list[at];
			settled = 1;
			if ((entry->ife_flags & (IN6_IFF_TENTATIVE | IN6_IFF_DUPLICATED)) != 0U)
				settled = 0;
			usable = resolver_usable6(entry->ife_addr.s6_addr, settled);
			if (usable)
				*have6 = 1;
		}
	}

	/* Succeeded: the families found. */
	close(descriptor);
	return 0;
}

/*
 * Writes a port's service: its name in /etc/services for the protocol
 * (udp with NI_DGRAM, tcp otherwise) unless NI_NUMERICSERV, else its
 * number.  Returns 0 or EAI_OVERFLOW.
 */
static int
gai_service_name(
	uint16_t port,
	int flags,
	char *service,
	socklen_t service_length)
{
	const struct servent *entry;
	const char *protocol;
	int needed;

	/* Its name, unless the number was asked for. */
	entry = NULL;
	if ((flags & NI_NUMERICSERV) == 0) {
		protocol = "tcp";
		if ((flags & NI_DGRAM) != 0)
			protocol = "udp";
		entry = getservbyport((int)htons(port), protocol);
	}

	/* The name found, or the number. */
	if (entry != NULL && entry->s_name != NULL) {
		needed = snprintf(service, service_length, "%s", entry->s_name);
	} else {
		needed = snprintf(service, service_length, "%u", (unsigned)port);
	}

	/* Refuses a field too short for it. */
	if (needed < 0 || (socklen_t)needed >= service_length)
		return EAI_OVERFLOW;

	/* Succeeded: written. */
	return 0;
}

/*
 * Puts the IPv6 addresses before the IPv4 ones when the host has a source
 * that reaches the first IPv6 one (RFC 6724, resolver_inet6_preferred),
 * after them otherwise; each family keeps its order.
 */
static void
gai_order(
	struct gai_list *list)
{
	struct gai_address sorted[GAI_ADDRESSES];
	struct sockaddr_storage source;
	const struct sockaddr_in6 *source6;
	unsigned index;
	unsigned count;
	int first6;
	int have4;
	int have6;
	int pass;
	int found;
	int preferred;
	int family;

	/* Both families there, the first IPv6 one. */
	have4 = 0;
	have6 = 0;
	first6 = -1;
	for (index = 0; index < list->count; index++) {
		if (list->items[index].family == AF_INET)
			have4 = 1;
		if (list->items[index].family == AF_INET6 && first6 < 0)
			first6 = (int)index;
	}
	have6 = first6 >= 0;
	if (!have4 || !have6)
		return;

	/* Whether its source reaches it. */
	found = gai_source(&list->items[first6], &source);
	preferred = 0;
	if (found && source.ss_family == AF_INET6) {
		source6 = (const struct sockaddr_in6 *)&source;
		preferred = resolver_inet6_preferred(list->items[first6].address6.s6_addr, source6->sin6_addr.s6_addr);
	}

	/* The preferred family first, each in its order. */
	count = 0;
	for (pass = 0; pass < 2; pass++) {
		family = AF_INET;
		if ((pass == 0) == (preferred != 0))
			family = AF_INET6;
		for (index = 0; index < list->count; index++) {
			if (list->items[index].family == family)
				sorted[count++] = list->items[index];
		}
	}

	/* Succeeded: the new order. */
	memcpy(list->items, sorted, count * sizeof(sorted[0]));
}

/* Makes the addrinfo records of the list; 0, or EAI_MEMORY. */
static int
gai_build(
	const struct gai_list *list,
	uint16_t port,
	int socktype,
	int protocol,
	int flags,
	struct addrinfo **output)
{
	const struct gai_address *source;
	struct sockaddr_in *address;
	struct sockaddr_in6 *address6;
	struct addrinfo *head;
	struct addrinfo **tail;
	struct addrinfo *item;
	unsigned index;

	/* Each address. */
	head = NULL;
	tail = &head;
	for (index = 0; index < list->count; index++) {
		source = &list->items[index];
		item = calloc(1, sizeof(*item));
		if (item == NULL) {
			freeaddrinfo(head);
			return EAI_MEMORY;
		}
		*tail = item;
		tail = &item->ai_next;

		/* Its socket's address, IPv6 or IPv4. */
		if (source->family == AF_INET6) {
			address6 = calloc(1, sizeof(*address6));
			if (address6 == NULL) {
				freeaddrinfo(head);
				return EAI_MEMORY;
			}
			address6->sin6_family = AF_INET6;
			address6->sin6_port = htons(port);
			address6->sin6_addr = source->address6;
			address6->sin6_scope_id = source->scope;
			item->ai_addrlen = sizeof(*address6);
			item->ai_addr = (struct sockaddr *)address6;
		} else {
			address = calloc(1, sizeof(*address));
			if (address == NULL) {
				freeaddrinfo(head);
				return EAI_MEMORY;
			}
			address->sin_family = AF_INET;
			address->sin_port = htons(port);
			address->sin_addr = source->address;
			item->ai_addrlen = sizeof(*address);
			item->ai_addr = (struct sockaddr *)address;
		}

		/* What every record carries; the canonical name on the first. */
		item->ai_flags = flags;
		item->ai_family = source->family;
		item->ai_socktype = socktype;
		item->ai_protocol = protocol;
		if ((flags & AI_CANONNAME) != 0 && index == 0)
			item->ai_canonname = strdup(list->canonical);
	}

	/* Succeeded: the records. */
	*output = head;
	return 0;
}

/* Writes an IPv6 address as a number, a link-local one's zone after "%" (its interface's name, or its number with NI_NUMERICSCOPE). */
static int
gni_numeric6(
	const struct sockaddr_in6 *inet6,
	int flags,
	char *host,
	socklen_t host_length)
{
	char text[INET6_ADDRSTRLEN + IF_NAMESIZE + 2];
	char zone[IF_NAMESIZE];
	const char *written;
	char *named;
	size_t length;
	int link;

	/* The address. */
	written = inet_ntop(AF_INET6, &inet6->sin6_addr, text, sizeof(text));
	if (written == NULL)
		return EAI_SYSTEM;

	/* A link-local one's zone. */
	link = IN6_IS_ADDR_LINKLOCAL(&inet6->sin6_addr);
	if (link && inet6->sin6_scope_id != 0U) {
		named = NULL;
		if ((flags & NI_NUMERICSCOPE) == 0)
			named = if_indextoname(inet6->sin6_scope_id, zone);
		length = strlen(text);
		if (named != NULL)
			(void)snprintf(text + length, sizeof(text) - length, "%%%s", zone);
		else
			(void)snprintf(text + length, sizeof(text) - length, "%%%u", (unsigned)inet6->sin6_scope_id);
	}

	/* Succeeded when it fits. */
	length = strlen(text);
	if (length + 1U > host_length)
		return EAI_OVERFLOW;
	memcpy(host, text, length + 1U);
	return 0;
}

/* Supports the query id operation. */
static uint16_t
query_id(
	const char *name)
{
	struct timespec now;
	uint32_t hash;

	(void)pthread_mutex_lock(&resolver_counter_lock);
	hash = ++resolver_counter;
	(void)pthread_mutex_unlock(&resolver_counter_lock);

	/* Continue while the operation condition remains true. */
	while (*name != '\0')
		hash = hash * 33U ^ (uint8_t)*name++;

	/* Handles a failed clock gettime operation. */
	if (clock_gettime(CLOCK_MONOTONIC, &now) == 0)
		hash ^= (uint32_t)now.tv_nsec ^ (uint32_t)now.tv_sec;

	/* Returns the computed result. */
	return (uint16_t)(hash ^ hash >> 16);
}

/* Supports the tcp query operation. */
static int
tcp_query(
	const struct sockaddr *server,
	socklen_t server_length,
	const uint8_t *query,
	size_t query_length,
	uint16_t id,
	const char *name,
	uint16_t type,
	struct resolver_result *result)
{
	uint8_t request[514], response[2048], prefix[2];
	uint16_t length;
	int descriptor, error, truncated;

	request[0] = (uint8_t)(query_length >> 8);
	request[1] = (uint8_t)query_length;
	memcpy(request + 2, query, query_length);
	descriptor = socket(server->sa_family, SOCK_STREAM, IPPROTO_TCP);

	/* Checks the file descriptor. */
	if (descriptor < 0)
		return EAI_AGAIN;

	/* Handles a failed connect operation. */
	if (connect(descriptor, server, server_length) != 0 ||
	    write_all_socket(descriptor, request, query_length + 2U) != 0 ||
	    read_exact_socket(descriptor, prefix, 2U) != 0) {
		close(descriptor);

		/* Returns the computed result. */
		return EAI_AGAIN;
	}
	length = (uint16_t)((uint16_t)prefix[0] << 8 | prefix[1]);

	/* Handles a failed read exact socket operation. */
	if (length > sizeof(response) ||
	    read_exact_socket(descriptor, response, length) != 0) {
		close(descriptor);

		/* Returns the computed result. */
		return EAI_FAIL;
	}
	close(descriptor);
	error = resolver_dns_parse(response, length, id, name, type, result,
				   &truncated);

	/* Returns the computed result. */
	return truncated ? EAI_FAIL : error;
}

/* Supports the write all socket operation. */
static int
write_all_socket(
	int descriptor,
	const uint8_t *buffer,
	size_t length)
{
	ssize_t count;

	/* Process each remaining element. */
	while (length != 0U) {
		count = send(descriptor, buffer, length, 0);

		/* Checks the remaining item count. */
		if (count <= 0)
			return -1;
		buffer += count;
		length -= (size_t)count;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the read exact socket operation. */
static int
read_exact_socket(
	int descriptor,
	uint8_t *buffer,
	size_t length)
{
	ssize_t count;

	/* Process each remaining element. */
	while (length != 0U) {
		count = recv(descriptor, buffer, length, 0);

		/* Checks the remaining item count. */
		if (count <= 0)
			return -1;
		buffer += count;
		length -= (size_t)count;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the parse service operation. */
static int
parse_service(
	const char *service,
	const char *protocol,
	uint16_t *port)
{
	char *end;
	unsigned long value;

	/* Handles the service availability. */
	if (service == NULL) {
		*port = 0;
		/* Reports successful completion. */
		return 0;
	}
	value = strtoul(service, &end, 10);

	/* A name that is not a number is looked up in the service database. */
	if (*service == '\0' || *end != '\0') {
		const struct servent *entry;

		entry = getservbyname(service, protocol);
		if (entry == NULL)
			return EAI_SERVICE;
		*port = ntohs((uint16_t)entry->s_port);

		/* Reports successful completion. */
		return 0;
	}

	/* Handles the service condition. */
	if (value > 65535U)
		return EAI_SERVICE;
	*port = (uint16_t)value;
	/* Reports successful completion. */
	return 0;
}

/* Supports the make ptr name operation. */
static int
make_ptr_name(
	struct in_addr address,
	char *output,
	size_t capacity)
{
	int function_result;
	uint32_t value;

	value = ntohl(address.s_addr);

	/* Computes the function result. */
	function_result = snprintf(output, capacity, "%u.%u.%u.%u.in-addr.arpa",
			value & 255U, value >> 8 & 255U, value >> 16 & 255U,
			value >> 24 & 255U) >= (int)capacity
		   ? EAI_OVERFLOW
		   : 0;

	/* Returns the computed result. */
	return function_result;
}

/* ------------------------------------------------------------------ *
 * The service database
 *
 * /etc/services is read line by line.  A returned entry points into
 * per-thread storage that the next call on the same thread reuses, which is
 * what the historical interface promises.
 * ------------------------------------------------------------------ */

#define SERVICE_ALIAS_MAX 8
#define SERVICE_LINE_MAX 256

struct service_state {
	FILE *file;
	int keep_open;
	struct servent entry;
	char name[64];
	char proto[16];
	char alias_text[SERVICE_ALIAS_MAX][64];
	char *aliases[SERVICE_ALIAS_MAX + 1];
};

static __thread struct service_state service_state;

/*
 * Supports the field splitting operation.
 *
 * Returns the next blank-separated field and advances the cursor past it.
 * This is strtok_r's job, which the C library does not have yet.
 */
static char *
next_field(
	char **cursor)
{
	char *text, *start;

	text = *cursor;

	/* Continue while the operation condition remains true. */
	while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')
		text++;

	/* Reports that no result is available. */
	if (*text == '\0') {
		*cursor = text;
		return NULL;
	}
	start = text;

	/* Continue while the operation condition remains true. */
	while (*text != '\0' && *text != ' ' && *text != '\t' &&
	       *text != '\r' && *text != '\n')
		text++;

	/* Terminates the field unless the line already ended. */
	if (*text != '\0')
		*text++ = '\0';
	*cursor = text;

	/* Returns the computed result. */
	return start;
}

/* Supports the service state operation. */
static struct service_state *
service_context(void)
{
	/* Returns the computed result. */
	return &service_state;
}

/* Supports the service open operation. */
static int
service_open(
	struct service_state *state)
{
	/* Handles the already open condition. */
	if (state->file != NULL)
		return 0;
	state->file = fopen("/etc/services", "r");

	/* Handles the file availability. */
	if (state->file == NULL)
		return -1;

	/* Reports successful completion. */
	return 0;
}

/*
 * Reads the next entry into the thread's storage.
 *
 * A line is "name port/protocol [alias...]"; anything from a '#' is a
 * comment.  A malformed line is skipped rather than ending the walk.
 */
static struct servent *
service_next(
	struct service_state *state)
{
	char line[SERVICE_LINE_MAX];
	char *text, *field, *slash, *end;
	unsigned long port;
	unsigned count;

	/* Process input until it is exhausted. */
	while (fgets(line, sizeof(line), state->file) != NULL) {
		text = strchr(line, '#');
		if (text != NULL)
			*text = '\0';

		/* Takes the service name. */
		text = line;
		field = next_field(&text);
		if (field == NULL)
			continue;
		if (strlen(field) >= sizeof(state->name))
			continue;
		strcpy(state->name, field);

		/* Takes the port and protocol, which share one field. */
		field = next_field(&text);
		if (field == NULL)
			continue;
		slash = strchr(field, '/');
		if (slash == NULL)
			continue;
		*slash++ = '\0';
		port = strtoul(field, &end, 10);
		if (*field == '\0' || *end != '\0' || port > 65535U)
			continue;
		if (strlen(slash) >= sizeof(state->proto))
			continue;
		strcpy(state->proto, slash);

		/* Takes any further names for the same entry. */
		count = 0;
		while (count < SERVICE_ALIAS_MAX) {
			field = next_field(&text);
			if (field == NULL)
				break;
			if (strlen(field) >= sizeof(state->alias_text[0]))
				continue;
			strcpy(state->alias_text[count], field);
			state->aliases[count] = state->alias_text[count];
			count++;
		}
		state->aliases[count] = NULL;

		state->entry.s_name = state->name;
		state->entry.s_aliases = state->aliases;
		state->entry.s_port = (int)htons((uint16_t)port);
		state->entry.s_proto = state->proto;

		/* Returns the computed result. */
		return &state->entry;
	}

	/* Reports that no result is available. */
	return NULL;
}

/*
 * Implements the setservent operation.
 */
void
setservent(int keep_open)
{
	struct service_state *state;

	state = service_context();
	state->keep_open = keep_open != 0;

	/* Restarts the walk when the database is already open. */
	if (state->file != NULL)
		rewind(state->file);
	else
		(void)service_open(state);
}

/*
 * Implements the endservent operation.
 */
void
endservent(void)
{
	struct service_state *state;

	state = service_context();

	/* Handles the file availability. */
	if (state->file != NULL) {
		(void)fclose(state->file);
		state->file = NULL;
	}
	state->keep_open = 0;
}

/*
 * Implements the getservent operation.
 */
struct servent *
getservent(void)
{
	struct service_state *state;

	state = service_context();

	/* Handles a failed open operation. */
	if (service_open(state) != 0)
		return NULL;

	/* Returns the computed result. */
	return service_next(state);
}

/* Supports the service lookup operation. */
static struct servent *
service_find(
	const char *name,
	int port,
	const char *protocol)
{
	struct service_state *state;
	struct servent *entry;
	unsigned index;
	int matched;

	state = service_context();

	/* Handles a failed open operation. */
	if (service_open(state) != 0)
		return NULL;
	rewind(state->file);

	/* Process each remaining element. */
	while ((entry = service_next(state)) != NULL) {
		/* Skips an entry for a different protocol. */
		if (protocol != NULL && strcmp(entry->s_proto, protocol) != 0)
			continue;

		/* Selects by name, including the further names. */
		if (name != NULL) {
			matched = strcmp(entry->s_name, name) == 0;
			for (index = 0; !matched &&
			     entry->s_aliases[index] != NULL; index++)
				matched = strcmp(entry->s_aliases[index],
						 name) == 0;
			if (!matched)
				continue;
		} else if (entry->s_port != port) {
			continue;
		}

		/* Leaves the database open only when asked to. */
		if (!state->keep_open) {
			(void)fclose(state->file);
			state->file = NULL;
		}

		/* Returns the computed result. */
		return entry;
	}

	/* Leaves the database open only when asked to. */
	if (!state->keep_open) {
		(void)fclose(state->file);
		state->file = NULL;
	}

	/* Reports that no result is available. */
	return NULL;
}

/*
 * Implements the getservbyname operation.
 */
struct servent *
getservbyname(const char *name, const char *protocol)
{
	/* Handles the name availability. */
	if (name == NULL)
		return NULL;

	/* Returns the computed result. */
	return service_find(name, 0, protocol);
}

/*
 * Implements the getservbyport operation.
 */
struct servent *
getservbyport(int port, const char *protocol)
{
	/* Returns the computed result. */
	return service_find(NULL, port, protocol);
}

/* ------------------------------------------------------------------ *
 * The host database
 *
 * These are the interfaces POSIX.1-2008 removed.  They are answered from the
 * same resolver getaddrinfo uses, so there is one name service and not two,
 * and the result lives in per-thread storage the next call reuses.
 * ------------------------------------------------------------------ */

#define HOST_ADDRESS_MAX 8

struct host_state {
	int error;
	struct hostent entry;
	char name[256];
	struct in_addr addresses[HOST_ADDRESS_MAX];
	char *address_list[HOST_ADDRESS_MAX + 1];
	char *aliases[1];
};

static __thread struct host_state host_state;

/*
 * Implements the h_errno location operation.
 */
int *
__h_errno_location(void)
{
	/* Returns the computed result. */
	return &host_state.error;
}

/*
 * Implements the hstrerror operation.
 */
const char *
hstrerror(int error)
{
	/* Selects the matching description. */
	switch (error) {
	case 0:
		return "Resolver error 0";
	case HOST_NOT_FOUND:
		return "Unknown host";
	case TRY_AGAIN:
		return "Host name lookup failure";
	case NO_RECOVERY:
		return "Unknown server error";
	case NO_DATA:
		return "No address associated with name";
	default:
		break;
	}

	/* Returns the computed result. */
	return "Unknown resolver error";
}

/*
 * Implements the sethostent operation.
 *
 * There is no host file to hold open, so the request is accepted and the
 * resolver is consulted per call.
 */
void
sethostent(int keep_open)
{
	(void)keep_open;
}

/*
 * Implements the endhostent operation.
 */
void
endhostent(void)
{
}

/* Supports the host error translation operation. */
static int
host_error_of(
	int error)
{
	/* Selects the matching description. */
	switch (error) {
	case EAI_NONAME:
	case EAI_ADDRFAMILY:
		return HOST_NOT_FOUND;
	case EAI_AGAIN:
		return TRY_AGAIN;
	case EAI_MEMORY:
	case EAI_SYSTEM:
	case EAI_FAIL:
		return NO_RECOVERY;
	default:
		break;
	}

	/* Returns the computed result. */
	return NO_RECOVERY;
}

/* Supports the host entry publication operation. */
static struct hostent *
host_publish(
	struct host_state *state,
	const char *name,
	unsigned count)
{
	unsigned index;

	/* Handles the absence of any address. */
	if (count == 0) {
		state->error = HOST_NOT_FOUND;
		return NULL;
	}

	/* Process each remaining element. */
	for (index = 0; index < count; index++)
		state->address_list[index] = (char *)&state->addresses[index];
	state->address_list[count] = NULL;
	state->aliases[0] = NULL;

	/* Keeps the reported name inside the thread's storage. */
	if (name != NULL && name != state->name) {
		if (strlen(name) >= sizeof(state->name)) {
			state->error = NO_RECOVERY;
			return NULL;
		}
		strcpy(state->name, name);
	}

	state->entry.h_name = state->name;
	state->entry.h_aliases = state->aliases;
	state->entry.h_addrtype = AF_INET;
	state->entry.h_length = (int)sizeof(struct in_addr);
	state->entry.h_addr_list = state->address_list;
	state->error = 0;

	/* Returns the computed result. */
	return &state->entry;
}

/*
 * Implements the gethostbyname operation.
 */
struct hostent *
gethostbyname(const char *name)
{
	struct host_state *state;
	struct addrinfo hints;
	struct addrinfo *list;
	struct addrinfo *entry;
	struct in_addr literal;
	unsigned count;
	int error;

	state = &host_state;

	/* Rejects a missing name. */
	if (name == NULL) {
		state->error = HOST_NOT_FOUND;
		return NULL;
	}

	/* An address in text form is its own answer. */
	if (inet_aton(name, &literal) != 0) {
		state->addresses[0] = literal;

		/* Returns the computed result. */
		return host_publish(state, name, 1);
	}

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	list = NULL;
	error = getaddrinfo(name, NULL, &hints, &list);

	/* Handles a failed lookup. */
	if (error != 0) {
		state->error = host_error_of(error);
		return NULL;
	}

	/* Collects the addresses the resolver returned. */
	count = 0;
	for (entry = list; entry != NULL && count < HOST_ADDRESS_MAX;
	     entry = entry->ai_next) {
		if (entry->ai_family != AF_INET || entry->ai_addr == NULL)
			continue;
		state->addresses[count++] =
		    ((const struct sockaddr_in *)(const void *)
			entry->ai_addr)->sin_addr;
	}

	/* Prefers the canonical name the resolver reported. */
	if (list != NULL && list->ai_canonname != NULL &&
	    strlen(list->ai_canonname) < sizeof(state->name))
		strcpy(state->name, list->ai_canonname);
	else if (strlen(name) < sizeof(state->name))
		strcpy(state->name, name);
	else
		count = 0;
	freeaddrinfo(list);

	/* Returns the computed result. */
	return host_publish(state, state->name, count);
}

/*
 * Implements the gethostbyaddr operation.
 */
struct hostent *
gethostbyaddr(const void *address, socklen_t length, int family)
{
	struct host_state *state;
	struct sockaddr_in query;
	char name[256];
	int error;

	state = &host_state;

	/* Only IPv4 addresses can be looked up. */
	if (address == NULL || family != AF_INET ||
	    length != sizeof(struct in_addr)) {
		state->error = HOST_NOT_FOUND;
		return NULL;
	}
	memcpy(&state->addresses[0], address, sizeof(struct in_addr));

	memset(&query, 0, sizeof(query));
	query.sin_family = AF_INET;
	query.sin_addr = state->addresses[0];
	error = getnameinfo((const struct sockaddr *)&query, sizeof(query),
	    name, (socklen_t)sizeof(name), NULL, 0, NI_NAMEREQD);

	/* Handles a failed reverse lookup. */
	if (error != 0) {
		state->error = host_error_of(error);
		return NULL;
	}

	/* Returns the computed result. */
	return host_publish(state, name, 1);
}
