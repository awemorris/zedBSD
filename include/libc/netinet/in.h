/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_NETINET_IN_H
#define LIBC_NETINET_IN_H

#include <uapi/netinet.h>

/*
 * The IPv6 address tests POSIX puts in this header.  Each takes a pointer to
 * a struct in6_addr and looks only at its bytes, so it holds in any byte
 * order.
 */
#define __IN6_BYTE(a, i)	(((const struct in6_addr *)(a))->s6_addr[i])
#define __IN6_ZERO(a, from, to)						\
	(__IN6_BYTE(a, from) == 0 && __IN6_BYTE(a, (from) + 1) == 0 &&	\
	 __IN6_BYTE(a, (from) + 2) == 0 && __IN6_BYTE(a, (from) + 3) == 0 && \
	 ((to) - (from) < 8 || (__IN6_BYTE(a, (from) + 4) == 0 &&	\
	  __IN6_BYTE(a, (from) + 5) == 0 && __IN6_BYTE(a, (from) + 6) == 0 && \
	  __IN6_BYTE(a, (from) + 7) == 0)))

#define IN6_IS_ADDR_UNSPECIFIED(a)					\
	(__IN6_ZERO(a, 0, 8) && __IN6_ZERO(a, 8, 16))
#define IN6_IS_ADDR_LOOPBACK(a)						\
	(__IN6_ZERO(a, 0, 8) && __IN6_ZERO(a, 8, 12) &&			\
	 __IN6_BYTE(a, 12) == 0 && __IN6_BYTE(a, 13) == 0 &&		\
	 __IN6_BYTE(a, 14) == 0 && __IN6_BYTE(a, 15) == 1)
#define IN6_IS_ADDR_MULTICAST(a)	(__IN6_BYTE(a, 0) == 0xff)
#define IN6_IS_ADDR_LINKLOCAL(a)					\
	(__IN6_BYTE(a, 0) == 0xfe && (__IN6_BYTE(a, 1) & 0xc0) == 0x80)
#define IN6_IS_ADDR_SITELOCAL(a)					\
	(__IN6_BYTE(a, 0) == 0xfe && (__IN6_BYTE(a, 1) & 0xc0) == 0xc0)
#define IN6_IS_ADDR_V4MAPPED(a)						\
	(__IN6_ZERO(a, 0, 8) && __IN6_BYTE(a, 8) == 0 &&		\
	 __IN6_BYTE(a, 9) == 0 && __IN6_BYTE(a, 10) == 0xff &&		\
	 __IN6_BYTE(a, 11) == 0xff)
#define IN6_IS_ADDR_V4COMPAT(a)						\
	(__IN6_ZERO(a, 0, 8) && __IN6_ZERO(a, 8, 12) &&			\
	 !(__IN6_BYTE(a, 12) == 0 && __IN6_BYTE(a, 13) == 0 &&		\
	   __IN6_BYTE(a, 14) == 0 && __IN6_BYTE(a, 15) <= 1))
#define __IN6_MC_SCOPE(a)	(__IN6_BYTE(a, 1) & 0x0f)
#define IN6_IS_ADDR_MC_NODELOCAL(a)					\
	(IN6_IS_ADDR_MULTICAST(a) && __IN6_MC_SCOPE(a) == 0x1)
#define IN6_IS_ADDR_MC_LINKLOCAL(a)					\
	(IN6_IS_ADDR_MULTICAST(a) && __IN6_MC_SCOPE(a) == 0x2)
#define IN6_IS_ADDR_MC_SITELOCAL(a)					\
	(IN6_IS_ADDR_MULTICAST(a) && __IN6_MC_SCOPE(a) == 0x5)
#define IN6_IS_ADDR_MC_ORGLOCAL(a)					\
	(IN6_IS_ADDR_MULTICAST(a) && __IN6_MC_SCOPE(a) == 0x8)
#define IN6_IS_ADDR_MC_GLOBAL(a)					\
	(IN6_IS_ADDR_MULTICAST(a) && __IN6_MC_SCOPE(a) == 0xe)

#endif
