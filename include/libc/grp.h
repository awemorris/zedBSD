/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_GRP_H
#define LIBC_GRP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <sys/types.h>

struct group {
	char *gr_name;
	char *gr_passwd;
	gid_t gr_gid;
	char **gr_mem;
};

struct group *getgrnam(const char *);
struct group *getgrgid(gid_t);

/* Collects every group a user belongs to, starting with the one given. */
int getgrouplist(const char *, gid_t, gid_t *, int *);

/* Names a group, or reports its number as text when there is no name. */
const char *group_from_gid(gid_t, int);
int getgrnam_r(const char *, struct group *, char *, size_t, struct group **);
int getgrgid_r(gid_t, struct group *, char *, size_t, struct group **);
void setgrent(void);
struct group *getgrent(void);
void endgrent(void);
int initgroups(const char *, gid_t);

#ifdef __cplusplus
}
#endif

#endif
