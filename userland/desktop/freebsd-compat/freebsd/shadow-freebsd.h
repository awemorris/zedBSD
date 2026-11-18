/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The subset of <shadow.h> passkey and its password check use, on FreeBSD
 * (ws172-p011).  FreeBSD keeps the password hash in master.passwd, which
 * getpwnam_r gives to root; getspnam_r here gives the same account in the
 * shadow entry's shape.  Only the fields those callers read are filled:
 * the name, the hash and the expiry (in days since 1970, -1 for none);
 * the others are -1.
 */
#ifndef KEILAND_SHADOW_FREEBSD_H
#define KEILAND_SHADOW_FREEBSD_H

#include <stddef.h>

/* An account's shadow entry, as glibc's struct spwd. */
struct spwd {
	char *sp_namp;
	char *sp_pwdp;
	long sp_lstchg;
	long sp_min;
	long sp_max;
	long sp_warn;
	long sp_inact;
	long sp_expire;
	unsigned long sp_flag;
};

int getspnam_r(const char *name, struct spwd *entry, char *buffer, size_t size, struct spwd **result);

#endif
