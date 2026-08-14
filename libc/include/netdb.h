/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_NETDB_H
#define ZEDBSD_NETDB_H

#include <sys/socket.h>

struct addrinfo {
	int ai_flags;
	int ai_family;
	int ai_socktype;
	int ai_protocol;
	socklen_t ai_addrlen;
	struct sockaddr *ai_addr;
	char *ai_canonname;
	struct addrinfo *ai_next;
};

#define AI_PASSIVE       0x0001
#define AI_CANONNAME     0x0002
#define AI_NUMERICHOST   0x0004
#define AI_NUMERICSERV   0x0008

#define NI_NUMERICHOST   0x0001
#define NI_NUMERICSERV   0x0002
#define NI_NAMEREQD      0x0004

#define EAI_ADDRFAMILY  (-1)
#define EAI_AGAIN       (-2)
#define EAI_BADFLAGS    (-3)
#define EAI_FAIL        (-4)
#define EAI_FAMILY      (-5)
#define EAI_MEMORY      (-6)
#define EAI_NONAME      (-7)
#define EAI_SERVICE     (-8)
#define EAI_SOCKTYPE    (-9)
#define EAI_SYSTEM      (-10)
#define EAI_OVERFLOW    (-11)

int getaddrinfo(const char *, const char *, const struct addrinfo *,
	struct addrinfo **);
void freeaddrinfo(struct addrinfo *);
const char *gai_strerror(int);
int getnameinfo(const struct sockaddr *, socklen_t, char *, socklen_t,
	char *, socklen_t, int);

#endif
