/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The operating system's name, defined in this one place.
 *
 * The distribution's name may change apart from the kernel's (zedBSD is
 * the kernel's name and, for now, the system's too), so a program that
 * names the system takes it from here instead of spelling it.  The build
 * reads the same two values for /etc/os-release (config/os-name.mk).  A
 * program that describes the system it runs on reads NAME from
 * /etc/os-release at run time instead.
 */

#ifndef KERN_OS_NAME_H
#define KERN_OS_NAME_H

/* The name people read: os-release's NAME. */
#define ZEDBSD_OS_NAME "zedBSD"

/* The name programs compare: os-release's ID, lower case. */
#define ZEDBSD_OS_ID "zedbsd"

#endif
