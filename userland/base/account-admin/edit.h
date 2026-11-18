/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * account-admin's pure part (ws089-p026, ws196-p002; docs/architecture/
 * security.md, "Account administration"): the request read from standard
 * input in either of its two forms (Settings', and root's for useradd,
 * usermod and userdel), the rules of names and values, the count of the
 * administrators who can log in, the choice of a new user ID, and the
 * edits of the texts of /etc/passwd, /etc/group and /etc/shadow.  Nothing
 * here touches a file, so the host tests run it alone; main.c reads the
 * files under the lock, checks, edits and writes.
 */

#ifndef ACCOUNT_ADMIN_EDIT_H
#define ACCOUNT_ADMIN_EDIT_H

#include <stddef.h>

/* The longest request taken, without its end. */
#define ADMIN_REQUEST_MAX	4096U

/*
 * The most lines a request has: Settings' password, operation and four
 * arguments, or root's operation and one line for each of its keys.
 */
#define ADMIN_LINES_MAX		10U

/* The longest name of a user, and the first and the last user ID given to a person. */
#define ADMIN_NAME_MAX		32U
#define ADMIN_ID_FIRST		1000L
#define ADMIN_ID_LAST		59999L

/* The longest shell path taken, and the longest hash field edited. */
#define ADMIN_SHELL_MAX		255U
#define ADMIN_HASH_FIELD_MAX	255U

/*
 * The operations: Settings' six, which a caller who is not root asks for
 * with its password, then root's three (ws196-p002), which only a caller
 * whose real user ID is 0 asks for, without a password.
 */
enum admin_operation {
	ADMIN_ADD,
	ADMIN_REMOVE,
	ADMIN_RESET_PASSWORD,
	ADMIN_GROUP_ADD,
	ADMIN_GROUP_REMOVE,
	ADMIN_SYSTEM_LANGUAGE,
	ADMIN_USER_ADD,
	ADMIN_USER_MODIFY,
	ADMIN_USER_REMOVE
};

/*
 * What a root's modification does to the password: nothing, a lock (a
 * '!' before the hash), or the lock taken off.
 */
enum admin_lock {
	ADMIN_LOCK_KEEP,
	ADMIN_LOCK_SET,
	ADMIN_LOCK_CLEAR
};

/*
 * Why a request is refused: each has its fixed word on the answer line
 * (admin_reason_word), which Settings shows in the user's language.
 */
enum admin_reason {
	ADMIN_OK,
	ADMIN_BAD_REQUEST,
	ADMIN_NOT_ADMINISTRATOR,
	ADMIN_BAD_PASSWORD,
	ADMIN_NO_SUCH_USER,
	ADMIN_NAME_TAKEN,
	ADMIN_BAD_NAME,
	ADMIN_WEAK_PASSWORD,
	ADMIN_LAST_ADMINISTRATOR,
	ADMIN_SELF,
	ADMIN_ROOT,
	ADMIN_BUSY,
	ADMIN_HOME_EXISTS,
	ADMIN_FAILED,
	ADMIN_BAD_VALUE,
	ADMIN_ID_TAKEN,
	ADMIN_NO_PASSWORD,
	ADMIN_HOME_FAILED
};

/*
 * A request, its strings pointing into the request's text (each line's end
 * replaced by a NUL).
 *
 * Settings' form gives the caller's password, the operation, and its
 * arguments (the name; the display name or keep-home/remove-home, the new
 * password, or the group; the new password of an addition; admin or
 * user).  The system language's one argument (ws158-p004: "en" or "ja",
 * the language of the login screen) is held as the name.
 *
 * Root's form has no password: the name, and what its keys gave -- the
 * display name (comment), the shell, the user ID (-1 when none was
 * given), the primary group, the list of groups (replacing the user's
 * groups, or added to them), the lock, whether a home is made or removed,
 * and whether a busy user is removed all the same.  A string not given is
 * NULL.
 */
struct admin_request {
	char *password;
	enum admin_operation operation;
	int by_root;
	const char *name;
	const char *display;
	const char *fresh;
	const char *group;
	const char *shell;
	const char *primary;
	const char *groups;
	long uid;
	enum admin_lock lock;
	int groups_append;
	int administrator;
	int make_home;
	int remove_home;
	int force;
};

int admin_parse(char *text, size_t length, int by_root, struct admin_request *request);
const char *admin_reason_word(int reason);
const char *admin_operation_word(enum admin_operation operation);
int admin_name_valid(const char *name);
int admin_display_valid(const char *display);
int admin_shell_valid(const char *shell);
int admin_groups_valid(const char *groups, int may_be_empty);
int admin_language_valid(const char *language);
int admin_field(const char *text, size_t length, const char *name, unsigned index, const char **field, size_t *field_length);
long admin_number_field(const char *text, size_t length, const char *name, unsigned index);
int admin_number_used(const char *text, size_t length, unsigned index, long number);
long admin_next_id(const char *passwd, size_t passwd_length, const char *group, size_t group_length, long floor);
int admin_field_set(const char *text, size_t length, const char *name, unsigned index, const char *value, char *output, size_t capacity, size_t *written);
int admin_lock_hash(const char *shadow, size_t length, const char *name, enum admin_lock lock, char *output, size_t capacity, size_t *written);
int admin_line_remove(const char *text, size_t length, const char *name, char *output, size_t capacity, size_t *written);
int admin_line_append(const char *text, size_t length, const char *line, char *output, size_t capacity, size_t *written);
int admin_member_set(const char *text, size_t length, const char *group, const char *name, int member, char *output, size_t capacity, size_t *written);
int admin_member_forget(const char *text, size_t length, const char *name, char *output, size_t capacity, size_t *written);
int admin_in_group(const char *group_text, size_t group_length, const char *group, const char *name, long primary);
int admin_group_removable(const char *group, size_t group_length, const char *passwd, size_t passwd_length, const char *name, long uid, long gid);
size_t admin_administrators(const char *passwd, size_t passwd_length, const char *group, size_t group_length, const char *shadow, size_t shadow_length, long day);

#endif
