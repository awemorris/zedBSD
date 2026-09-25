/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_ARPA_INET_H
#define LIBC_ARPA_INET_H

#ifdef __cplusplus
extern "C" {
#endif

#include <netinet/in.h>

#define INET_ADDRSTRLEN 16

int inet_aton(const char *text, struct in_addr *address);
char *inet_ntoa(struct in_addr address);
uint32_t inet_addr(const char *text);
int inet_pton(int family, const char *text, void *address);
const char *inet_ntop(int family, const void *address, char *text, socklen_t length);

#ifdef __cplusplus
}
#endif

#endif
