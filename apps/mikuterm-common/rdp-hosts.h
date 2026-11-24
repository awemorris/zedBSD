/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Parser and formatter for mikuterm's rdp/hosts configuration file and
 * target host specifications.
 */

#ifndef MIKUTERM_RDP_HOSTS_H
#define MIKUTERM_RDP_HOSTS_H

#include <stddef.h>
#include <stdint.h>

/* Maximum number of RDP host blocks read from rdp/hosts. */
#define RDP_HOSTS_MAX		32U

/*
 * One RDP host profile parsed from rdp/hosts or a typed target string.
 */
struct rdp_host_entry {
	char name[128];
	char host_name[256];
	uint16_t port;
	char user[128];
	char domain[128];
	char password[128];
	uint32_t width;
	uint32_t height;
};

/*
 * Collection of RDP host profiles read from rdp/hosts.
 */
struct rdp_host_list {
	struct rdp_host_entry entries[RDP_HOSTS_MAX];
	unsigned count;
};

int rdp_hosts_parse(const char *text, size_t size, uint16_t default_port,
		    struct rdp_host_list *list);
int rdp_hosts_format(const struct rdp_host_list *list, char **out,
		     size_t *out_size);
int rdp_hosts_parse_target(const char *spec, uint16_t default_port,
			   struct rdp_host_entry *out);

#endif
