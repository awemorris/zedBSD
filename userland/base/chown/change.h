/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares the ownership change that chown and chgrp share.
 */

#ifndef USERLAND_BASE_CHOWN_CHANGE_H
#define USERLAND_BASE_CHOWN_CHANGE_H

#include <sys/types.h>

/* Symbolic links are changed themselves and not followed (-P). */
#define CHANGE_FOLLOW_NONE 0

/* Symbolic links named as operands are followed (-H). */
#define CHANGE_FOLLOW_OPERANDS 1

/* Every symbolic link is followed (-L). */
#define CHANGE_FOLLOW_ALL 2

/*
 * One run of chown or chgrp.
 *
 * uid and gid are (uid_t)-1 and (gid_t)-1 for an ID that stays as it is.
 * follow matters only with recursive; without it, no_dereference (-h)
 * says whether a symbolic link operand is changed itself.
 */
struct owner_change {
	const char *program;
	uid_t uid;
	gid_t gid;
	int recursive;
	int follow;
	int no_dereference;
};

int owner_read_options(int argc, char **argv, struct owner_change *change);
int owner_change_operand(const struct owner_change *change, const char *path);
int owner_parse_user(const char *text, uid_t *uid, gid_t *login_group);
int owner_parse_group(const char *text, gid_t *gid);

#endif
