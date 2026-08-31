/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD C library account support.
 */

#include <pwd.h>
#include <grp.h>
#include <shadow.h>

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ACCOUNT_LINE_MAX 1024
#define ACCOUNT_RESULT_MAX 2048
#define ACCOUNT_GROUP_MAX 16

static pthread_mutex_t account_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *passwd_stream, *group_stream, *shadow_stream;
static _Thread_local struct passwd passwd_result;
static _Thread_local struct group group_result;
static _Thread_local struct spwd shadow_result;
static _Thread_local char passwd_buffer[ACCOUNT_RESULT_MAX];
static _Thread_local char group_buffer[ACCOUNT_RESULT_MAX];
static _Thread_local char shadow_buffer[ACCOUNT_RESULT_MAX];

static int passwd_lookup(const char *name, uid_t uid, int by_name, struct passwd *entry, char *buffer, size_t size, struct passwd **result);
static int passwd_parse(const char *line, struct passwd *entry, char *buffer, size_t size);
static unsigned split_fields(char *line, char **field, unsigned maximum);
static int parse_id(const char *text, uint32_t *value);
static int group_lookup(const char *name, gid_t gid, int by_name, struct group *entry, char *buffer, size_t size, struct group **result);
static int group_parse(const char *line, struct group *entry, char *buffer, size_t size);
static int shadow_parse(const char *line, struct spwd *entry, char *buffer, size_t size);
static long shadow_number(const char *s, unsigned long empty);

/*
 * Implements the getpwnam r operation.
 */
int
getpwnam_r(
	const char *name,
	struct passwd *entry,
	char *buffer,
	size_t size,
	struct passwd **result)
{
	int function_result;

	/* Handles the name availability. */
	if (name == NULL || entry == NULL || buffer == NULL || result == NULL)
		return EINVAL;

	/* Obtains the passwd lookup result. */
	function_result = passwd_lookup(name, 0, 1, entry, buffer, size, result);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the getpwuid r operation.
 */
int
getpwuid_r(
	uid_t uid,
	struct passwd *entry,
	char *buffer,
	size_t size,
	struct passwd **result)
{
	int function_result;

	/* Handles the entry availability. */
	if (entry == NULL || buffer == NULL || result == NULL)
		return EINVAL;

	/* Obtains the passwd lookup result. */
	function_result = passwd_lookup(NULL, uid, 0, entry, buffer, size, result);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the getpwnam operation.
 */
struct passwd *
getpwnam(
	const char *name)
{
	struct passwd *result;
	int error;

	error = getpwnam_r(name, &passwd_result, passwd_buffer,
			       sizeof(passwd_buffer), &result);

	/* Handles an operation failure. */
	if (error != 0)
		errno = error;

	/* Returns the computed result. */
	return error == 0 ? result : NULL;
}

/*
 * Implements the getpwuid operation.
 */
struct passwd *
getpwuid(
	uid_t uid)
{
	struct passwd *result;
	int error;

	error = getpwuid_r(uid, &passwd_result, passwd_buffer,
			       sizeof(passwd_buffer), &result);

	/* Handles an operation failure. */
	if (error != 0)
		errno = error;

	/* Returns the computed result. */
	return error == 0 ? result : NULL;
}

/*
 * Implements the setpwent operation.
 */
void
setpwent(
	void)
{
	pthread_mutex_lock(&account_lock);

	/* Handles the passwd stream availability. */
	if (passwd_stream != NULL)
		fclose(passwd_stream);
	passwd_stream = fopen("/etc/passwd", "r");
	pthread_mutex_unlock(&account_lock);
}

/*
 * Implements the getpwent operation.
 */
struct passwd *
getpwent(
	void)
{
	char line[ACCOUNT_LINE_MAX];
	struct passwd *result;

	result = NULL;
	pthread_mutex_lock(&account_lock);

	/* Handles the passwd stream availability. */
	if (passwd_stream == NULL)

	/* Process input until it is exhausted. */
		passwd_stream = fopen("/etc/passwd", "r");
	while (passwd_stream != NULL &&
	       fgets(line, sizeof(line), passwd_stream)) {
		/* Handles a failed passwd parse operation. */
		if (line[0] != '#' && line[0] != '\n' &&
		    passwd_parse(line, &passwd_result, passwd_buffer,
				 sizeof(passwd_buffer)) == 0) {
			result = &passwd_result;
			break;
		}
	}
	pthread_mutex_unlock(&account_lock);

	/* Returns the computed result. */
	return result;
}

/*
 * Implements the endpwent operation.
 */
void
endpwent(
	void)
{
	pthread_mutex_lock(&account_lock);

	/* Handles the passwd stream availability. */
	if (passwd_stream != NULL)
		fclose(passwd_stream);
	passwd_stream = NULL;
	pthread_mutex_unlock(&account_lock);
}

/*
 * Implements the getgrnam r operation.
 */
int
getgrnam_r(
	const char *n,
	struct group *g,
	char *b,
	size_t z,
	struct group **r)
{
	int function_result;

	/* Checks the current item count. */
	if (!n || !g || !b || !r)
		return EINVAL;

	/* Obtains the group lookup result. */
	function_result = group_lookup(n, 0, 1, g, b, z, r);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the getgrgid r operation.
 */
int
getgrgid_r(
	gid_t id,
	struct group *g,
	char *b,
	size_t z,
	struct group **r)
{
	int function_result;

	/* Handles the g condition. */
	if (!g || !b || !r)
		return EINVAL;

	/* Obtains the group lookup result. */
	function_result = group_lookup(NULL, id, 0, g, b, z, r);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the getgrnam operation.
 */
struct group *
getgrnam(
	const char *n)
{
	struct group *r;
	int e;

	e = getgrnam_r(n, &group_result, group_buffer, sizeof(group_buffer),
			   &r);

	/* Handles the e condition. */
	if (e)
		errno = e;

	/* Returns the computed result. */
	return e ? NULL : r;
}

/*
 * Implements the getgrgid operation.
 */
struct group *
getgrgid(
	gid_t id)
{
	struct group *r;
	int e;

	e = getgrgid_r(id, &group_result, group_buffer,
			   sizeof(group_buffer), &r);

	/* Handles the e condition. */
	if (e)
		errno = e;

	/* Returns the computed result. */
	return e ? NULL : r;
}

/*
 * Implements the setgrent operation.
 */
void
setgrent(
	void)
{
	pthread_mutex_lock(&account_lock);

	/* Handles the group stream condition. */
	if (group_stream)
		fclose(group_stream);
	group_stream = fopen("/etc/group", "r");
	pthread_mutex_unlock(&account_lock);
}

/*
 * Implements the getgrent operation.
 */
struct group *
getgrent(
	void)
{
	char line[ACCOUNT_LINE_MAX];
	struct group *r;

	r = NULL;
	pthread_mutex_lock(&account_lock);

	/* Handles the group stream condition. */
	if (!group_stream)

	/* Process input until it is exhausted. */
		group_stream = fopen("/etc/group", "r");
	while (group_stream && fgets(line, sizeof(line), group_stream)) {
		/* Handles a failed group parse operation. */
		if (line[0] != '#' && line[0] != '\n' &&
		    group_parse(line, &group_result, group_buffer,
				sizeof(group_buffer)) == 0) {
			r = &group_result;
			break;
		}
	}
	pthread_mutex_unlock(&account_lock);

	/* Returns the computed result. */
	return r;
}

/*
 * Implements the endgrent operation.
 */
void
endgrent(
	void)
{
	pthread_mutex_lock(&account_lock);

	/* Handles the group stream condition. */
	if (group_stream)
		fclose(group_stream);
	group_stream = NULL;
	pthread_mutex_unlock(&account_lock);
}

/*
 * Implements the initgroups operation.
 */
int
initgroups(
	const char *user,
	gid_t primary)
{
	int function_result;
	unsigned i;
	int duplicate;
	FILE *stream;
	char line[ACCOUNT_LINE_MAX], buffer[ACCOUNT_RESULT_MAX];
	struct group entry;
	gid_t groups[ACCOUNT_GROUP_MAX];
	size_t count;

	count = 1;

	/* Handles the user condition. */
	if (!user) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	groups[0] = primary;
	stream = fopen("/etc/group", "r");

	/* Handles the stream condition. */
	if (!stream)
		return -1;

	/* Process input until it is exhausted. */
	while (fgets(line, sizeof(line), stream)) {
		duplicate = 0;

		/* Handles a failed group parse operation. */
		if (group_parse(line, &entry, buffer, sizeof(buffer)) != 0)
			continue;

		/* Process each element required by the operation. */
		for (i = 0; entry.gr_mem[i]; i++) {
			/* Selects the matching value. */
			if (!strcmp(entry.gr_mem[i], user))
				break;
		}

		/* Handles the entry condition. */
		if (!entry.gr_mem[i])
			continue;

		/* Process each remaining element. */
		for (i = 0; i < count; i++) {
			/* Handles the groups condition. */
			if (groups[i] == entry.gr_gid)
				duplicate = 1;
		}

		/* Handles the duplicate condition. */
		if (!duplicate) {
			/* Checks the remaining item count. */
			if (count == ACCOUNT_GROUP_MAX) {
				fclose(stream);
				errno = E2BIG;

				/* Reports operation failure. */
				return -1;
			}
			groups[count++] = entry.gr_gid;
		}
	}
	fclose(stream);

	/* Obtains the setgroups result. */
	function_result = setgroups(count, groups);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the getspnam r operation.
 */
int
getspnam_r(
	const char *name,
	struct spwd *entry,
	char *buffer,
	size_t size,
	struct spwd **result)
{
	FILE *stream;
	char line[ACCOUNT_LINE_MAX];
	int error;

	error = 0;
	*result = NULL;
	/* Validates the current name. */
	if (!name || !entry || !buffer || !result)
		return EINVAL;
	stream = fopen("/etc/shadow", "r");

	/* Handles the stream condition. */
	if (!stream)
		return errno;

	/* Process input until it is exhausted. */
	while (fgets(line, sizeof(line), stream)) {
		/* Handles the line condition. */
		if (line[0] == '#' || line[0] == '\n')
			continue;
		error = shadow_parse(line, entry, buffer, size);

		/* Handles an operation failure. */
		if (error == EINVAL)
			continue;

		/* Handles an operation failure. */
		if (error || !strcmp(entry->sp_namp, name)) {
			/* Handles an operation failure. */
			if (!error)
				*result = entry;
			break;
		}
	}

	/* Handles an operation failure. */
	if (ferror(stream) && !error)
		error = EIO;
	fclose(stream);

	/* Returns the computed result. */
	return error;
}

/*
 * Implements the getspnam operation.
 */
struct spwd *
getspnam(
	const char *n)
{
	struct spwd *r;
	int e;

	e = getspnam_r(n, &shadow_result, shadow_buffer,
			   sizeof(shadow_buffer), &r);

	/* Handles the e condition. */
	if (e)
		errno = e;

	/* Returns the computed result. */
	return e ? NULL : r;
}

/*
 * Implements the setspent operation.
 */
void
setspent(
	void)
{
	pthread_mutex_lock(&account_lock);

	/* Handles the shadow stream condition. */
	if (shadow_stream)
		fclose(shadow_stream);
	shadow_stream = fopen("/etc/shadow", "r");
	pthread_mutex_unlock(&account_lock);
}

/*
 * Implements the getspent operation.
 */
struct spwd *
getspent(
	void)
{
	char line[ACCOUNT_LINE_MAX];
	struct spwd *r;

	r = NULL;
	pthread_mutex_lock(&account_lock);

	/* Handles the shadow stream condition. */
	if (!shadow_stream)

	/* Process input until it is exhausted. */
		shadow_stream = fopen("/etc/shadow", "r");
	while (shadow_stream && fgets(line, sizeof(line), shadow_stream)) {
		/* Handles a failed shadow parse operation. */
		if (line[0] != '#' && line[0] != '\n' &&
		    shadow_parse(line, &shadow_result, shadow_buffer,
				 sizeof(shadow_buffer)) == 0) {
			r = &shadow_result;
			break;
		}
	}
	pthread_mutex_unlock(&account_lock);

	/* Returns the computed result. */
	return r;
}

/*
 * Implements the endspent operation.
 */
void
endspent(
	void)
{
	pthread_mutex_lock(&account_lock);

	/* Handles the shadow stream condition. */
	if (shadow_stream)
		fclose(shadow_stream);
	shadow_stream = NULL;
	pthread_mutex_unlock(&account_lock);
}

/* Supports the passwd lookup operation. */
static int
passwd_lookup(
	const char *name,
	uid_t uid,
	int by_name,
	struct passwd *entry,
	char *buffer,
	size_t size,
	struct passwd **result)
{
	FILE *stream;
	char line[ACCOUNT_LINE_MAX];
	int error;

	error = 0;

	*result = NULL;
	stream = fopen("/etc/passwd", "r");

	/* Handles the stream availability. */
	if (stream == NULL)
		return errno;

	/* Process input until it is exhausted. */
	while (fgets(line, sizeof(line), stream) != NULL) {
		/* Handles the line condition. */
		if (line[0] == '#' || line[0] == '\n')
			continue;
		error = passwd_parse(line, entry, buffer, size);

		/* Handles an operation failure. */
		if (error == EINVAL)
			continue;

		/* Handles an operation failure. */
		if (error != 0)
			break;

		/* Handles the by name condition. */
		if ((by_name && !strcmp(entry->pw_name, name)) ||
		    (!by_name && entry->pw_uid == uid)) {
			*result = entry;
			break;
		}
	}

	/* Handles an operation failure. */
	if (ferror(stream) && error == 0)
		error = EIO;
	fclose(stream);

	/* Returns the computed result. */
	return error;
}

/* Supports the passwd parse operation. */
static int
passwd_parse(
	const char *line,
	struct passwd *entry,
	char *buffer,
	size_t size)
{
	char *field[7];
	size_t length;
	uint32_t uid, gid;

	length = strlen(line) + 1U;

	/* Checks the current data length. */
	if (length > size)
		return ERANGE;
	memcpy(buffer, line, length);

	/* Handles a failed split fields operation. */
	if (split_fields(buffer, field, 7) != 7 || parse_id(field[2], &uid) ||
	    parse_id(field[3], &gid))

		/* Returns the computed result. */
		return EINVAL;
	entry->pw_name = field[0];
	entry->pw_passwd = field[1];
	entry->pw_uid = (uid_t)uid;
	entry->pw_gid = (gid_t)gid;
	entry->pw_gecos = field[4];
	entry->pw_dir = field[5];
	entry->pw_shell = field[6];

	/* Reports successful completion. */
	return 0;
}

/* Supports the split fields operation. */
static unsigned
split_fields(
	char *line,
	char **field,
	unsigned maximum)
{
	unsigned count;
	char *cursor;

	count = 0;
	cursor = line;

	/* Process each remaining element. */
	while (count < maximum) {
		field[count++] = cursor;
		cursor = strchr(cursor, ':');

		/* Handles the cursor availability. */
		if (cursor == NULL)
			break;
		*cursor++ = '\0';
	}

	/* Handles the cursor availability. */
	if (cursor != NULL)
		return 0;

	/* Checks the remaining item count. */
	if (count != 0) {
		/* Continue while the operation condition remains true. */
		cursor = field[count - 1];
		while (*cursor != '\0' && *cursor != '\r' && *cursor != '\n')
			cursor++;
		*cursor = '\0';
	}

	/* Returns the computed result. */
	return count;
}

/* Supports the parse id operation. */
static int
parse_id(
	const char *text,
	uint32_t *value)
{
	char *end;
	unsigned long number;

	/* Handles the text availability. */
	if (text == NULL || *text == '\0')
		return EINVAL;
	errno = 0;
	number = strtoul(text, &end, 10);

	/* Handles the reported system error. */
	if (errno != 0 || *end != '\0' || number > UINT_MAX)
		return EINVAL;
	*value = (uint32_t)number;
	/* Reports successful completion. */
	return 0;
}

/* Supports the group lookup operation. */
static int
group_lookup(
	const char *name,
	gid_t gid,
	int by_name,
	struct group *entry,
	char *buffer,
	size_t size,
	struct group **result)
{
	FILE *stream;
	char line[ACCOUNT_LINE_MAX];
	int error;

	stream = fopen("/etc/group", "r");
	error = 0;
	*result = NULL;
	/* Handles the stream availability. */
	if (stream == NULL)
		return errno;

	/* Process input until it is exhausted. */
	while (fgets(line, sizeof(line), stream)) {
		/* Handles the line condition. */
		if (line[0] == '#' || line[0] == '\n')
			continue;
		error = group_parse(line, entry, buffer, size);

		/* Handles an operation failure. */
		if (error == EINVAL)
			continue;

		/* Handles an operation failure. */
		if (error != 0)
			break;

		/* Handles the by name condition. */
		if ((by_name && !strcmp(entry->gr_name, name)) ||
		    (!by_name && entry->gr_gid == gid)) {
			*result = entry;
			break;
		}
	}

	/* Handles an operation failure. */
	if (ferror(stream) && error == 0)
		error = EIO;
	fclose(stream);

	/* Returns the computed result. */
	return error;
}

/* Supports the group parse operation. */
static int
group_parse(
	const char *line,
	struct group *entry,
	char *buffer,
	size_t size)
{
	unsigned index;
	char *field[4], *cursor;
	char **members;
	size_t length, strings, aligned, slots;
	uint32_t gid;

	length = strlen(line) + 1U;
	slots = 1U;

	/* Checks the current data length. */
	if (length > size)
		return ERANGE;
	memcpy(buffer, line, length);

	/* Handles a failed split fields operation. */
	if (split_fields(buffer, field, 4) != 4 || parse_id(field[2], &gid))
		return EINVAL;

	/* Process each element required by the operation. */
	for (cursor = field[3]; *cursor != '\0'; cursor++) {
		/* Checks the current cursor position. */
		if (*cursor == ',')
			slots++;
	}

	/* Handles the field condition. */
	if (field[3][0] == '\0')
		slots = 0;
	strings = length;
	aligned = (strings + sizeof(char *) - 1U) & ~(sizeof(char *) - 1U);

	/* Handles the aligned condition. */
	if (aligned > size || (slots + 1U) > (size - aligned) / sizeof(char *))
		return ERANGE;
	members = (char **)(void *)(buffer + aligned);

	/* Handles the slots condition. */
	if (slots != 0) {
		/* Continue until the operation reaches a terminal state. */
		index = 0;
		cursor = field[3];
		while (1) {
			members[index++] = cursor;
			cursor = strchr(cursor, ',');

			/* Handles the cursor availability. */
			if (cursor == NULL)
				break;
			*cursor++ = '\0';
		}
		members[index] = NULL;
	} else {
		members[0] = NULL;
	}
	entry->gr_name = field[0];
	entry->gr_passwd = field[1];
	entry->gr_gid = (gid_t)gid;
	entry->gr_mem = members;

	/* Reports successful completion. */
	return 0;
}

/* Supports the shadow parse operation. */
static int
shadow_parse(
	const char *line,
	struct spwd *entry,
	char *buffer,
	size_t size)
{
	char *field[9];
	size_t length;

	length = strlen(line) + 1U;

	/* Checks the current data length. */
	if (length > size)
		return ERANGE;
	memcpy(buffer, line, length);

	/* Handles a failed split fields operation. */
	if (split_fields(buffer, field, 9) != 9)
		return EINVAL;
	entry->sp_namp = field[0];
	entry->sp_pwdp = field[1];
	entry->sp_lstchg = shadow_number(field[2], (unsigned long)-1L);
	entry->sp_min = shadow_number(field[3], (unsigned long)-1L);
	entry->sp_max = shadow_number(field[4], (unsigned long)-1L);
	entry->sp_warn = shadow_number(field[5], (unsigned long)-1L);
	entry->sp_inact = shadow_number(field[6], (unsigned long)-1L);
	entry->sp_expire = shadow_number(field[7], (unsigned long)-1L);
	entry->sp_flag = (unsigned long)shadow_number(field[8], ~0UL);

	/* Reports successful completion. */
	return 0;
}

/* Supports the shadow number operation. */
static long
shadow_number(
	const char *s,
	unsigned long empty)
{
	char *end;
	long value;

	/* Checks the current string state. */
	if (!s || !*s)
		return (long)empty;
	errno = 0;
	value = strtol(s, &end, 10);

	/* Returns the computed result. */
	return errno || *end ? -1L : value;
}
