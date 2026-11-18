/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_NET_IF_H
#define LIBC_NET_IF_H

#include <uapi/netif.h>

/* An interface's name and index (POSIX; ws130-p004). */
#define IF_NAMESIZE	IFNAMSIZ

unsigned if_nametoindex(const char *name);
char *if_indextoname(unsigned index, char *name);

/*
 * One interface in the list if_nameindex() returns: its index and its name.
 * The list ends with an entry whose index is 0 and whose name is NULL.
 */
struct if_nameindex {
	unsigned if_index;
	char *if_name;
};

struct if_nameindex *if_nameindex(void);
void if_freenameindex(struct if_nameindex *);

#endif
