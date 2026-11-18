/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares the zedBSD userland service config interface.
 */

#ifndef KERN_SERVICE_CONFIG_H
#define KERN_SERVICE_CONFIG_H

#include <stddef.h>

#define KERN_INIT_SOCKET "/run/init.sock"

/*
 * The mark sessiond leaves once a desktop (a session or the login screen)
 * has taken the display.  init removes it at each boot and, in a boot that
 * shows the desktop first, waits for it before it starts the late services.
 */
#define KERN_INIT_DESKTOP_SHOWN "/run/desktop-shown"

int assignment_get(const char *, const char *, char *, size_t);
int service_name_valid(const char *);

#endif
