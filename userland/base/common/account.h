/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The accounts' shared core (account.c, ws160-p001): what passwd, su and
 * sudo (and, through passwd, Settings) need besides login's password check
 * (userland/base/login/verify.c): the new password's rules, its SHA-512
 * crypt hash, the safe replacement of a user's hash in /etc/shadow,
 * membership of the wheel group, and the cleaned environment of a command
 * run as another user; and for account-admin (ws089-p026), the lock of the
 * account files and their whole-file read and atomic replacement.
 *
 * The pure parts (account_password_check, account_shadow_replace,
 * account_environment) touch no file, so the host tests run them alone.
 */

#ifndef USERLAND_BASE_COMMON_ACCOUNT_H
#define USERLAND_BASE_COMMON_ACCOUNT_H

#include <stddef.h>
#include <sys/types.h>

/*
 * passwd, and the exit statuses of its batch mode (passwd -s), which
 * programs that change a password through it read (ws160-p002).
 */
#ifndef ACCOUNT_PASSWD_PATH
#define ACCOUNT_PASSWD_PATH		"/bin/passwd"
#endif
#define ACCOUNT_PASSWD_OK		0
#define ACCOUNT_PASSWD_FAILED		1
#define ACCOUNT_PASSWD_USAGE		2
#define ACCOUNT_PASSWD_WRONG		3
#define ACCOUNT_PASSWD_REFUSED		4
#define ACCOUNT_PASSWD_MISMATCH		5

/* account-admin, the program that administers the accounts for the desktop's Settings (ws089-p026). */
#ifndef ACCOUNT_ADMIN_PATH
#define ACCOUNT_ADMIN_PATH		"/usr/libexec/account-admin"
#endif

/* The account files, and the lock all three change under (a host test names its own). */
#ifndef ACCOUNT_SHADOW_PATH
#define ACCOUNT_SHADOW_PATH		"/etc/shadow"
#endif
#ifndef ACCOUNT_PASSWD_FILE
#define ACCOUNT_PASSWD_FILE		"/etc/passwd"
#endif
#ifndef ACCOUNT_GROUP_FILE
#define ACCOUNT_GROUP_FILE		"/etc/group"
#endif
#ifndef ACCOUNT_SHADOW_LOCK
#define ACCOUNT_SHADOW_LOCK		"/etc/shadow.lock"
#endif

/* The longest path of an account file. */
#define ACCOUNT_PATH_MAX		256U

/* The shortest password a user may choose, the longest password taken, and the longest hash written. */
#define ACCOUNT_PASSWORD_MIN		8U
#define ACCOUNT_PASSWORD_MAX		256U
#define ACCOUNT_HASH_MAX		128U

/* The SHA-512 crypt rounds of a new hash. */
#define ACCOUNT_HASH_ROUNDS		65536U

/* Why a new password is refused (account_password_check). */
#define ACCOUNT_PASSWORD_OK		0
#define ACCOUNT_PASSWORD_SHORT		1
#define ACCOUNT_PASSWORD_LONG		2
#define ACCOUNT_PASSWORD_CHARACTER	3
#define ACCOUNT_PASSWORD_SAME		4

/* The secure PATH of a command run as another user. */
#define ACCOUNT_SECURE_PATH		"/bin:/sbin:/usr/bin:/usr/sbin"

/* The most variables account_environment writes. */
#define ACCOUNT_ENVIRONMENT_MAX		32U

int account_password_check(const char *password, const char *previous, int by_root);
const char *account_password_reason(int reason);
int account_password_hash(const char *password, char *hash, size_t size);
int account_shadow_replace(const char *text, size_t length, const char *name, const char *hash, long day, char *output, size_t capacity, size_t *written);
int account_shadow_set(const char *name, const char *hash);
int account_in_wheel(const char *name, gid_t primary);
int account_files_lock(void);
void account_files_unlock(void);
int account_file_read(const char *path, char *buffer, size_t capacity, size_t *length);
int account_file_write(const char *target, mode_t mode, const char *text, size_t length);

/*
 * What account_environment builds from: the caller's environment (kept
 * only for the variables of the terminal and the locale), the target's
 * home, shell and name, and the SUDO_* variables when sudo_user is set.
 */
struct account_environment_input {
	char *const *caller;
	const char *home;
	const char *shell;
	const char *user;
	const char *sudo_user;
	uid_t sudo_uid;
	gid_t sudo_gid;
	const char *sudo_command;
};

size_t account_environment(const struct account_environment_input *input, char *storage, size_t storage_size, char **variables, size_t capacity);

#endif
