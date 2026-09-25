/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_NETDB_H
#define LIBC_NETDB_H

#ifdef __cplusplus
extern "C" {
#endif

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

/*
 * Buffer sizes for getnameinfo.  These are not POSIX names, but portable
 * software sizes its host and service buffers with them, so the traditional
 * BSD values are kept.
 */
#define NI_MAXHOST       1025
#define NI_MAXSERV       32

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

/*
 * The host and service databases.
 *
 * POSIX.1-2008 removed the host functions in favour of getaddrinfo, and they
 * carry a per-thread error in h_errno rather than errno.  They are kept
 * because portable software still reaches for them, and because a name
 * service has to answer the question they ask.
 */
struct hostent {
	char *h_name;
	char **h_aliases;
	int h_addrtype;
	int h_length;
	char **h_addr_list;
};

#define h_addr h_addr_list[0]

#define HOST_NOT_FOUND 1
#define TRY_AGAIN      2
#define NO_RECOVERY    3
#define NO_DATA        4
#define NO_ADDRESS     NO_DATA

/*
 * h_errno is per-thread, so it is reached through a function the way errno
 * is.  Code that writes `h_errno` unchanged keeps working.
 */
int *__h_errno_location(void);
#define h_errno (*__h_errno_location())

struct hostent *gethostbyname(const char *);
struct hostent *gethostbyaddr(const void *, socklen_t, int);
void sethostent(int);
void endhostent(void);
const char *hstrerror(int);

/*
 * The service database, read from /etc/services.  s_port is in network byte
 * order, as the historical interface specifies.
 */
struct servent {
	char *s_name;
	char **s_aliases;
	int s_port;
	char *s_proto;
};

struct servent *getservbyname(const char *, const char *);
struct servent *getservbyport(int, const char *);
void setservent(int);
struct servent *getservent(void);
void endservent(void);

/*
 * A whole set of records of one type, which is what a caller checking a
 * signature or a fingerprint needs: the records and their signatures
 * together, with a word on whether the server said it had checked them.
 */
struct rdatainfo {
	unsigned int rdi_length;
	unsigned char *rdi_data;
};

struct rrsetinfo {
	unsigned int rri_flags;
	unsigned int rri_rdclass;
	unsigned int rri_rdtype;
	unsigned int rri_ttl;
	unsigned int rri_nrdatas;
	unsigned int rri_nsigs;
	char *rri_name;
	struct rdatainfo *rri_rdatas;
	struct rdatainfo *rri_sigs;
};

#define RRSET_VALIDATED 1	/* The server said it had checked these. */

#define ERRSET_SUCCESS  0
#define ERRSET_NOMEMORY 1
#define ERRSET_FAIL     2
#define ERRSET_INVAL    3
#define ERRSET_NONAME   4
#define ERRSET_NODATA   5

int getrrsetbyname(const char *, unsigned int, unsigned int, unsigned int,
	struct rrsetinfo **);
void freerrset(struct rrsetinfo *);

int getaddrinfo(const char *, const char *, const struct addrinfo *,
	struct addrinfo **);
void freeaddrinfo(struct addrinfo *);
const char *gai_strerror(int);
int getnameinfo(const struct sockaddr *, socklen_t, char *, socklen_t,
	char *, socklen_t, int);

#ifdef __cplusplus
}
#endif

#endif
