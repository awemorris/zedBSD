/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_SERVICE_CONFIG_H
#define ZEDBSD_SERVICE_CONFIG_H

#include <stddef.h>

#define ZEDBSD_INIT_SOCKET "/run/init.sock"

int assignment_get(const char *, const char *, char *, size_t);
int service_name_valid(const char *);

#endif
