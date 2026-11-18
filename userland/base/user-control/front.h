/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The part useradd, usermod and userdel share (ws196-p003;
 * docs/reference/user-administration.md): the check that root runs them,
 * root's request to account-admin built one key at a time, account-admin
 * run with it, and its answer turned into a message and an exit status.
 * The three commands only read their options; account-admin alone reads
 * and changes the account files.
 */

#ifndef USERLAND_BASE_USER_CONTROL_FRONT_H
#define USERLAND_BASE_USER_CONTROL_FRONT_H

#include <stddef.h>

/*
 * The exit statuses: shadow-utils' numbers where it has one for the same
 * case.
 */
#define USER_FRONT_OK		0
#define USER_FRONT_FAILED	1
#define USER_FRONT_USAGE	2
#define USER_FRONT_BAD_VALUE	3
#define USER_FRONT_ID_TAKEN	4
#define USER_FRONT_NO_SUCH_USER	6
#define USER_FRONT_BUSY		8
#define USER_FRONT_NAME_TAKEN	9
#define USER_FRONT_HOME		12

/* The longest request account-admin takes, without its end (its edit.h's ADMIN_REQUEST_MAX). */
#define USER_FRONT_REQUEST_MAX	4096U

/*
 * One request to account-admin being built: the command that sends it
 * (for its messages), and the text so far -- the operation's line, then
 * one "KEY VALUE" line for each key given.
 */
struct user_front_request {
	const char *program;
	char text[USER_FRONT_REQUEST_MAX];
	size_t length;
};

int user_front_start(const char *program);
void user_front_begin(struct user_front_request *request, const char *program, const char *operation);
int user_front_key(struct user_front_request *request, const char *key, const char *value);
int user_front_send(const struct user_front_request *request, const char *name);

#endif
