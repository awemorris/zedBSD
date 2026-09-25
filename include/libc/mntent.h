/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The mount table interface of glibc: setmntent(), getmntent(),
 * addmntent(), endmntent() and hasmntopt().
 *
 * zedBSD keeps no /etc/mtab; setmntent(MOUNTED, ...) asks the kernel for the
 * mounted file systems and hands back a real stream in mtab form, so that a
 * program written for the file works unchanged.
 */

#ifndef LIBC_MNTENT_H
#define LIBC_MNTENT_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOUNTED "/etc/mtab"
#define MNTTAB "/etc/fstab"

#define MNTTYPE_IGNORE "ignore"
#define MNTTYPE_NFS "nfs"
#define MNTTYPE_SWAP "swap"

#define MNTOPT_DEFAULTS "defaults"
#define MNTOPT_RO "ro"
#define MNTOPT_RW "rw"
#define MNTOPT_SUID "suid"
#define MNTOPT_NOSUID "nosuid"
#define MNTOPT_NOAUTO "noauto"

/* One line of the mount table. */
struct mntent {
	char *mnt_fsname;
	char *mnt_dir;
	char *mnt_type;
	char *mnt_opts;
	int mnt_freq;
	int mnt_passno;
};

FILE *setmntent(const char *path, const char *mode);
struct mntent *getmntent(FILE *stream);
int addmntent(FILE *stream, const struct mntent *entry);
int endmntent(FILE *stream);
char *hasmntopt(const struct mntent *entry, const char *option);

#ifdef __cplusplus
}
#endif

#endif
