/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_PATHS_H
#define LIBC_PATHS_H

/*
 * Where the things a program reaches for actually are.
 *
 * A program that spells these out cannot be moved; naming them once here is
 * what lets the layout change without touching the programs.
 */
#define _PATH_DEV        "/dev/"
#define _PATH_DEVNULL    "/dev/null"
#define _PATH_DEVZERO    "/dev/zero"
#define _PATH_TTY        "/dev/tty"
#define _PATH_CONSOLE    "/dev/console"
#define _PATH_MOUNTED    "/etc/mtab"
#define _PATH_MNTTAB     "/etc/fstab"
#define _PATH_URANDOM    "/dev/urandom"

#define _PATH_BSHELL     "/bin/sh"
#define _PATH_STDPATH    "/bin:/usr/bin:/sbin:/usr/sbin"
#define _PATH_DEFPATH    "/bin:/usr/bin"

#define _PATH_ETC        "/etc"
#define _PATH_PASSWD     "/etc/passwd"
#define _PATH_GROUP      "/etc/group"
#define _PATH_SHADOW     "/etc/shadow"
#define _PATH_SHELLS     "/etc/shells"
#define _PATH_NOLOGIN    "/etc/nologin"

#define _PATH_TMP        "/tmp/"
#define _PATH_VARTMP     "/var/tmp/"
#define _PATH_VARRUN     "/run/"
#define _PATH_VARDB      "/var/db/"
#define _PATH_MAILDIR    "/var/mail"
#define _PATH_MAN        "/usr/share/man"

#define _PATH_UTMPX      "/var/run/utmpx"
#define _PATH_WTMPX      "/var/log/wtmpx"
#define _PATH_LASTLOG    "/var/log/lastlog"

#endif
