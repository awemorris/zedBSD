/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_RESOLV_H
#define LIBC_RESOLV_H

#include <netdb.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Base 64, as the name service encodes binary records.
 */
int b64_ntop(const unsigned char *, size_t, char *, size_t);
int b64_pton(const char *, unsigned char *, size_t);

/*
 * Queries of the caller's own making.
 *
 * getaddrinfo answers the question "where is this name"; these answer any
 * question the protocol can carry, and hand back the reply as it arrived so
 * that the caller can read records this library knows nothing about.
 */
#define NS_PACKETSZ   512	/* The largest reply that fits in a datagram. */
#define NS_MAXDNAME   1025	/* The longest name, written out. */
#define NS_MAXCDNAME  255	/* The longest name, as it travels. */
#define NS_MAXLABEL   63	/* The longest single label. */
#define NS_HFIXEDSZ   12	/* The fixed part of a message header. */
#define NS_QFIXEDSZ   4		/* Type and class, after a question's name. */
#define NS_RRFIXEDSZ  10	/* Type, class, time to live and length. */
#define NS_INT32SZ    4
#define NS_INT16SZ    2

/* The older spellings, which portable software still uses. */
#define PACKETSZ      NS_PACKETSZ
#define MAXDNAME      NS_MAXDNAME
#define MAXCDNAME     NS_MAXCDNAME
#define MAXLABEL      NS_MAXLABEL
#define HFIXEDSZ      NS_HFIXEDSZ
#define QFIXEDSZ      NS_QFIXEDSZ
#define RRFIXEDSZ     NS_RRFIXEDSZ
#define INT32SZ       NS_INT32SZ
#define INT16SZ       NS_INT16SZ

/* Classes. */
#define C_IN   1
#define C_ANY  255
#define ns_c_in C_IN

/* The record types a caller is likely to ask for by name. */
#define T_A     1
#define T_NS    2
#define T_CNAME 5
#define T_SOA   6
#define T_PTR   12
#define T_MX    15
#define T_TXT   16
#define T_SIG   24
#define T_KEY   25
#define T_AAAA  28
#define T_SRV   33
#define T_NAPTR 35
#define T_CERT  37
#define T_DS    43
#define T_SSHFP 44
#define T_RRSIG 46
#define T_DNSKEY 48
#define T_TLSA  52
#define T_ANY   255

/* Operations, of which only a query is sent by this library. */
#define QUERY 0

/* Reply codes. */
#define NOERROR  0
#define FORMERR  1
#define SERVFAIL 2
#define NXDOMAIN 3
#define NOTIMP   4
#define REFUSED  5

#define MAXNS     3
#define MAXDNSRCH 6

/*
 * The resolver's own state.
 *
 * Software reads and writes these directly, which is why they are here
 * rather than behind a call: setting _res.nsaddr_list to reach a particular
 * server, or clearing RES_DNSRCH to stop a name being completed, is how the
 * interface has always been steered.
 */
struct __res_state {
	int retrans;			/* Seconds to wait for a reply. */
	int retry;			/* Attempts per server. */
	unsigned long options;
	int nscount;
	struct sockaddr_in nsaddr_list[MAXNS];
	unsigned short id;
	char defdname[256];		/* The local domain. */
	char *dnsrch[MAXDNSRCH + 1];	/* Domains an unqualified name tries. */
	char dnsrch_storage[MAXDNSRCH][256];
	int ndots;			/* Dots that make a name qualified. */
};

#define nsaddr nsaddr_list[0]

#define RES_INIT        0x00000001
#define RES_DEBUG       0x00000002
#define RES_USEVC       0x00000008	/* Ask over a stream, not a datagram. */
#define RES_IGNTC       0x00000020	/* Do not retry a truncated reply. */
#define RES_RECURSE     0x00000040
#define RES_DEFNAMES    0x00000080	/* Complete a name with one dot. */
#define RES_DNSRCH      0x00000200	/* Try each domain in the search list. */
#define RES_USE_EDNS0   0x00100000
#define RES_USE_DNSSEC  0x00200000

#define RES_DEFAULT (RES_RECURSE | RES_DEFNAMES | RES_DNSRCH)

extern struct __res_state _res;

int res_init(void);
int res_mkquery(int, const char *, int, int, const unsigned char *, int,
	const unsigned char *, unsigned char *, int);
int res_send(const unsigned char *, int, unsigned char *, int);
int res_query(const char *, int, int, unsigned char *, int);
int res_search(const char *, int, int, unsigned char *, int);
int res_querydomain(const char *, const char *, int, int, unsigned char *,
	int);

/* Reads a name out of a message, following the compression it uses. */
int dn_expand(const unsigned char *, const unsigned char *,
	const unsigned char *, char *, int);
int dn_comp(const char *, unsigned char *, int, unsigned char **,
	unsigned char **);
int dn_skipname(const unsigned char *, const unsigned char *);

unsigned int ns_get16(const unsigned char *);
unsigned long ns_get32(const unsigned char *);
void ns_put16(unsigned int, unsigned char *);
void ns_put32(unsigned long, unsigned char *);

#ifdef __cplusplus
}
#endif

#endif
