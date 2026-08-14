/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/libc/resolver-internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static uint32_t resolver_counter;

static uint16_t
query_id(const char *name)
{
	struct timespec now;
	uint32_t hash = ++resolver_counter;
	while (*name != '\0') hash = hash * 33U ^ (uint8_t)*name++;
	if (clock_gettime(CLOCK_MONOTONIC, &now) == 0)
		hash ^= (uint32_t)now.tv_nsec ^ (uint32_t)now.tv_sec;
	return (uint16_t)(hash ^ hash >> 16);
}

int
resolver_load_config(struct resolver_config *config)
{
	FILE *file;
	char line[128];

	if (config == NULL) return EAI_FAIL;
	memset(config, 0, sizeof(*config));
	file = fopen("/etc/resolv.cfg", "r");
	if (file == NULL) return EAI_AGAIN;
	while (config->count < DNS_MAX_NAMESERVERS &&
	    fgets(line, sizeof(line), file) != NULL) {
		char *text = line, *end;
		while (*text == ' ' || *text == '\t') text++;
		if (*text == '#' || *text == '\n' || *text == '\0') continue;
		if (strncmp(text, "nameserver", 10U) != 0 ||
		    (text[10] != ' ' && text[10] != '\t')) continue;
		text += 10;
		while (*text == ' ' || *text == '\t') text++;
		end = text;
		while (*end != '\0' && *end != '\n' && *end != '\r' &&
		    *end != ' ' && *end != '\t' && *end != '#') end++;
		*end = '\0';
		if (inet_aton(text, &config->servers[config->count]))
			config->count++;
	}
	fclose(file);
	return config->count != 0U ? 0 : EAI_AGAIN;
}

static int
write_all_socket(int descriptor, const uint8_t *buffer, size_t length)
{
	while (length != 0U) {
		ssize_t count = send(descriptor, buffer, length, 0);
		if (count <= 0) return -1;
		buffer += count;
		length -= (size_t)count;
	}
	return 0;
}

static int
read_exact_socket(int descriptor, uint8_t *buffer, size_t length)
{
	while (length != 0U) {
		ssize_t count = recv(descriptor, buffer, length, 0);
		if (count <= 0) return -1;
		buffer += count;
		length -= (size_t)count;
	}
	return 0;
}

static int
tcp_query(const struct sockaddr_in *server, const uint8_t *query,
	size_t query_length, uint16_t id, const char *name, uint16_t type,
	struct resolver_result *result)
{
	uint8_t request[514], response[2048], prefix[2];
	uint16_t length;
	int descriptor, error, truncated;

	request[0] = (uint8_t)(query_length >> 8);
	request[1] = (uint8_t)query_length;
	memcpy(request + 2, query, query_length);
	descriptor = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (descriptor < 0) return EAI_AGAIN;
	if (connect(descriptor, (const struct sockaddr *)server,
	    sizeof(*server)) != 0 ||
	    write_all_socket(descriptor, request, query_length + 2U) != 0 ||
	    read_exact_socket(descriptor, prefix, 2U) != 0) {
		close(descriptor);
		return EAI_AGAIN;
	}
	length = (uint16_t)((uint16_t)prefix[0] << 8 | prefix[1]);
	if (length > sizeof(response) ||
	    read_exact_socket(descriptor, response, length) != 0) {
		close(descriptor);
		return EAI_FAIL;
	}
	close(descriptor);
	error = resolver_dns_parse(response, length, id, name, type, result,
	    &truncated);
	return truncated ? EAI_FAIL : error;
}

static int
resolver_query_server_depth(const char *name, uint16_t type,
	const struct in_addr *server_address, uint16_t port,
	struct resolver_result *result, unsigned depth)
{
	uint8_t query[512], response[512];
	struct sockaddr_in server, source;
	struct timeval timeout;
	socklen_t source_length;
	size_t query_length;
	uint16_t id;
	int attempt, descriptor, error, truncated;

	if (name == NULL || server_address == NULL || result == NULL)
		return EAI_FAIL;
	memset(result, 0, sizeof(*result));
	id = query_id(name);
	error = resolver_dns_build_query(query, sizeof(query), id, name, type,
	    &query_length);
	if (error != 0) return error;
	memset(&server, 0, sizeof(server));
	server.sin_family = AF_INET;
	server.sin_port = htons(port);
	server.sin_addr = *server_address;
	for (attempt = 0; attempt < 2; attempt++) {
		descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (descriptor < 0) return EAI_SYSTEM;
		timeout.tv_sec = 2;
		timeout.tv_usec = 0;
		(void)setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO,
		    &timeout, sizeof(timeout));
		if (sendto(descriptor, query, query_length, 0,
		    (const struct sockaddr *)&server, sizeof(server)) >= 0) {
			source_length = sizeof(source);
			ssize_t count = recvfrom(descriptor, response, sizeof(response),
			    0, (struct sockaddr *)&source, &source_length);
			if (count >= 0 && source.sin_family == AF_INET &&
			    source.sin_addr.s_addr == server.sin_addr.s_addr &&
			    source.sin_port == server.sin_port) {
				error = resolver_dns_parse(response, (size_t)count, id,
				    name, type, result, &truncated);
				close(descriptor);
				if (truncated)
					error = tcp_query(&server, query, query_length,
					    id, name, type, result);
				if (error == EAI_NONAME && type == DNS_TYPE_A &&
				    result->canonical[0] != '\0' && depth < 8U) {
					struct resolver_result target;
					char alias[254];
					uint32_t cname_ttl = result->ttl;
					strncpy(alias, result->canonical, sizeof(alias) - 1U);
					alias[sizeof(alias) - 1U] = '\0';
					error = resolver_query_server_depth(alias, type,
					    server_address, port, &target, depth + 1U);
					if (error == 0) {
						*result = target;
						if (result->cname_count < 8U) {
							memmove(result->cname_chain + 1,
							    result->cname_chain,
							    result->cname_count * sizeof(result->cname_chain[0]));
							strncpy(result->cname_chain[0], alias, 253U);
							result->cname_count++;
						}
						if (cname_ttl != 0 &&
						    (result->ttl == 0 || cname_ttl < result->ttl))
							result->ttl = cname_ttl;
					}
				}
				if (error == 0) {
					result->server = *server_address;
					result->port = port;
				}
				return error;
			}
		}
		close(descriptor);
	}
	return EAI_AGAIN;
}

int
resolver_query_server(const char *name, uint16_t type,
	const struct in_addr *server_address, uint16_t port,
	struct resolver_result *result)
{
	return resolver_query_server_depth(name, type, server_address, port,
	    result, 0);
}

int
resolver_query(const char *name, uint16_t type, struct resolver_result *result)
{
	struct resolver_config config;
	unsigned index;
	int error;

	error = resolver_load_config(&config);
	if (error != 0) return error;
	for (index = 0; index < config.count; index++) {
		error = resolver_query_server(name, type, &config.servers[index],
		    53U, result);
		if (error == 0 || error == EAI_NONAME) return error;
	}
	return error;
}

static int
parse_service(const char *service, uint16_t *port)
{
	char *end;
	unsigned long value;
	if (service == NULL) { *port = 0; return 0; }
	value = strtoul(service, &end, 10);
	if (*service == '\0' || *end != '\0' || value > 65535U)
		return EAI_SERVICE;
	*port = (uint16_t)value;
	return 0;
}

int
getaddrinfo(const char *node, const char *service,
	const struct addrinfo *hints, struct addrinfo **output)
{
	struct resolver_result result;
	struct addrinfo *head = NULL, **tail = &head;
	struct in_addr numeric;
	uint16_t port;
	unsigned count, index;
	int family = AF_UNSPEC, socktype = 0, protocol = 0, flags = 0, error;

	if (output == NULL) return EAI_FAIL;
	*output = NULL;
	if (hints != NULL) {
		family = hints->ai_family; socktype = hints->ai_socktype;
		protocol = hints->ai_protocol; flags = hints->ai_flags;
		if ((flags & ~(AI_PASSIVE | AI_CANONNAME | AI_NUMERICHOST |
		    AI_NUMERICSERV)) != 0) return EAI_BADFLAGS;
	}
	if (family != AF_UNSPEC && family != AF_INET) return EAI_FAMILY;
	if (socktype != 0 && socktype != SOCK_DGRAM && socktype != SOCK_STREAM &&
	    socktype != SOCK_RAW) return EAI_SOCKTYPE;
	error = parse_service(service, &port);
	if (error != 0) return error;
	memset(&result, 0, sizeof(result));
	if (node == NULL) {
		numeric.s_addr = htonl((flags & AI_PASSIVE) ? INADDR_ANY : 0x7f000001U);
		result.addresses[0] = numeric; result.address_count = 1;
	} else if (inet_aton(node, &numeric)) {
		result.addresses[0] = numeric; result.address_count = 1;
		strncpy(result.canonical, node, sizeof(result.canonical) - 1U);
	} else {
		if ((flags & AI_NUMERICHOST) != 0) return EAI_NONAME;
		error = resolver_query(node, DNS_TYPE_A, &result);
		if (error != 0) return error;
		if (result.canonical[0] == '\0')
			strncpy(result.canonical, node, sizeof(result.canonical) - 1U);
	}
	count = result.address_count;
	for (index = 0; index < count; index++) {
		struct addrinfo *item = calloc(1, sizeof(*item));
		struct sockaddr_in *address = calloc(1, sizeof(*address));
		if (item == NULL || address == NULL) {
			free(item); free(address); freeaddrinfo(head); return EAI_MEMORY;
		}
		address->sin_family = AF_INET;
		address->sin_port = htons(port);
		address->sin_addr = result.addresses[index];
		item->ai_flags = flags; item->ai_family = AF_INET;
		item->ai_socktype = socktype; item->ai_protocol = protocol;
		item->ai_addrlen = sizeof(*address);
		item->ai_addr = (struct sockaddr *)address;
		if ((flags & AI_CANONNAME) != 0 && index == 0)
			item->ai_canonname = strdup(result.canonical);
		*tail = item; tail = &item->ai_next;
	}
	*output = head;
	return 0;
}

void
freeaddrinfo(struct addrinfo *info)
{
	while (info != NULL) {
		struct addrinfo *next = info->ai_next;
		free(info->ai_addr); free(info->ai_canonname); free(info);
		info = next;
	}
}

const char *
gai_strerror(int error)
{
	switch (error) {
	case 0: return "success";
	case EAI_AGAIN: return "temporary failure in name resolution";
	case EAI_BADFLAGS: return "invalid resolver flags";
	case EAI_FAIL: return "name server failure";
	case EAI_FAMILY: return "unsupported address family";
	case EAI_MEMORY: return "out of memory";
	case EAI_NONAME: return "name or service not known";
	case EAI_SERVICE: return "unsupported service";
	case EAI_SOCKTYPE: return "unsupported socket type";
	case EAI_OVERFLOW: return "result buffer too small";
	case EAI_SYSTEM: return "system error";
	default: return "resolver error";
	}
}

static int
make_ptr_name(struct in_addr address, char *output, size_t capacity)
{
	uint32_t value = ntohl(address.s_addr);
	return snprintf(output, capacity, "%u.%u.%u.%u.in-addr.arpa",
	    value & 255U, value >> 8 & 255U, value >> 16 & 255U,
	    value >> 24 & 255U) >= (int)capacity ? EAI_OVERFLOW : 0;
}

int
getnameinfo(const struct sockaddr *address, socklen_t length, char *host,
	socklen_t host_length, char *service, socklen_t service_length, int flags)
{
	const struct sockaddr_in *inet = (const struct sockaddr_in *)address;
	char buffer[254];
	struct resolver_result result;
	int error;

	if ((flags & ~(NI_NUMERICHOST | NI_NUMERICSERV | NI_NAMEREQD)) != 0)
		return EAI_BADFLAGS;
	if (address == NULL || length < sizeof(*inet) || inet->sin_family != AF_INET)
		return EAI_FAMILY;
	if (service != NULL && service_length != 0U) {
		int needed = snprintf(service, service_length, "%u", ntohs(inet->sin_port));
		if (needed < 0 || (socklen_t)needed >= service_length) return EAI_OVERFLOW;
	}
	if (host == NULL || host_length == 0U) return 0;
	if ((flags & NI_NUMERICHOST) == 0) {
		error = make_ptr_name(inet->sin_addr, buffer, sizeof(buffer));
		if (error == 0) error = resolver_query(buffer, DNS_TYPE_PTR, &result);
		if (error == 0) {
			if (strlen(result.ptr_name) + 1U > host_length) return EAI_OVERFLOW;
			strcpy(host, result.ptr_name); return 0;
		}
		if ((flags & NI_NAMEREQD) != 0) return error;
	}
	if (inet_ntop(AF_INET, &inet->sin_addr, buffer, sizeof(buffer)) == NULL)
		return EAI_SYSTEM;
	if (strlen(buffer) + 1U > host_length) return EAI_OVERFLOW;
	strcpy(host, buffer);
	return 0;
}
