/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The installed system's settings, written into its root after the units
 * are unpacked: the base unit's default user (uid 1000) replaced by the
 * plan's, both passwords set, the automatic login, the hostname, sshd
 * kept from root logins and the swap file in /etc/fstab.  The edits of
 * the files' texts touch no file, so the host tests run them alone; the
 * rest open each path one component at a time under the root, never
 * through a symbolic link, and replace a file by a rename.
 */

#ifndef USERLAND_DESKTOP_INSTALLER_ADMIN_TARGET_H
#define USERLAND_DESKTOP_INSTALLER_ADMIN_TARGET_H

#include "plan.h"

#include <stddef.h>

/* The uid and gid of the base unit's default user, which the plan's user takes. */
#define TARGET_USER_ID		1000U

/* The largest account or setting file the helper edits. */
#define TARGET_FILE_MAX		(64U * 1024U)

/* The installed files the helper edits, from the root. */
#define TARGET_PASSWD		"etc/passwd"
#define TARGET_SHADOW		"etc/shadow"
#define TARGET_GROUP		"etc/group"
#define TARGET_RC_CONF		"etc/rc.conf"
#define TARGET_FSTAB		"etc/fstab"
#define TARGET_SSHD_CONFIG	"etc/ssh/sshd_config"
#define TARGET_AUTOLOGIN	"etc/keiland/autologin"

/* The swap file's line of /etc/fstab. */
#define TARGET_SWAPFILE_LINE	"/swapfile none swap sw 0 0\n"

/* What a text edit reads and where it writes. */
struct target_edit {
	const char *text;
	size_t length;
	char *output;
	size_t capacity;
	size_t written;
};

int target_passwd_user(struct target_edit *edit, const char *name, const char *fullname, char *old_name, size_t old_capacity);
int target_record_rename(struct target_edit *edit, const char *old_name, const char *name);
int target_group_user(struct target_edit *edit, const char *old_name, const char *name);
int target_hostname(struct target_edit *edit, const char *hostname);
int target_sshd_no_root(struct target_edit *edit);
int target_fstab_swapfile(struct target_edit *edit);
int target_shadow_hash(const char *text, size_t length, const char *name, char *hash, size_t capacity);

int target_file_read(int root, const char *path, char *text, size_t capacity, size_t *length);
int target_file_replace(int root, const char *path, const char *text, size_t length);
int target_accounts(int root, const struct install_plan *plan);
int target_hostname_set(int root, const char *hostname);
int target_sshd_set(int root);
int target_fstab_set(int root);

#endif
