/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_ARPA_INET_H
#define ZEDBSD_ARPA_INET_H
#include <netinet/in.h>

#define INET_ADDRSTRLEN 16

int inet_aton(const char *text, struct in_addr *address);
uint32_t inet_addr(const char *text);
int inet_pton(int family, const char *text, void *address);
const char *inet_ntop(int family, const void *address, char *text,
		      socklen_t length);

#endif
