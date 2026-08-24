/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_SERVICE_CONFIG_H
#define ZEDBSD_SERVICE_CONFIG_H

#include <stddef.h>

#define ZEDBSD_RC_CONF "/etc/rc.conf"
#define ZEDBSD_INIT_SOCKET "/run/init.sock"

int rcconf_get(const char *, const char *, char *, size_t);
int rcconf_set_enabled(const char *, const char *, int);
int service_name_valid(const char *);

#endif
