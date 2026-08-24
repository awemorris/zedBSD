/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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

static unsigned
split_fields(char *line, char **field, unsigned maximum)
{
	unsigned count = 0;
	char *cursor = line;

	while (count < maximum) {
		field[count++] = cursor;
		cursor = strchr(cursor, ':');
		if (cursor == NULL)
			break;
		*cursor++ = '\0';
	}
	if (cursor != NULL)
		return 0;
	if (count != 0) {
		cursor = field[count - 1];
		while (*cursor != '\0' && *cursor != '\r' && *cursor != '\n')
			cursor++;
		*cursor = '\0';
	}
	return count;
}

static int
parse_id(const char *text, uint32_t *value)
{
	char *end;
	unsigned long number;

	if (text == NULL || *text == '\0')
		return EINVAL;
	errno = 0;
	number = strtoul(text, &end, 10);
	if (errno != 0 || *end != '\0' || number > UINT_MAX)
		return EINVAL;
	*value = (uint32_t)number;
	return 0;
}

static int
passwd_parse(const char *line, struct passwd *entry, char *buffer, size_t size)
{
	char *field[7];
	size_t length = strlen(line) + 1U;
	uint32_t uid, gid;

	if (length > size)
		return ERANGE;
	memcpy(buffer, line, length);
	if (split_fields(buffer, field, 7) != 7 || parse_id(field[2], &uid) ||
	    parse_id(field[3], &gid))
		return EINVAL;
	entry->pw_name = field[0];
	entry->pw_passwd = field[1];
	entry->pw_uid = (uid_t)uid;
	entry->pw_gid = (gid_t)gid;
	entry->pw_gecos = field[4];
	entry->pw_dir = field[5];
	entry->pw_shell = field[6];
	return 0;
}

static int
passwd_lookup(const char *name, uid_t uid, int by_name, struct passwd *entry,
	      char *buffer, size_t size, struct passwd **result)
{
	FILE *stream;
	char line[ACCOUNT_LINE_MAX];
	int error = 0;

	*result = NULL;
	stream = fopen("/etc/passwd", "r");
	if (stream == NULL)
		return errno;
	while (fgets(line, sizeof(line), stream) != NULL) {
		if (line[0] == '#' || line[0] == '\n')
			continue;
		error = passwd_parse(line, entry, buffer, size);
		if (error == EINVAL)
			continue;
		if (error != 0)
			break;
		if ((by_name && !strcmp(entry->pw_name, name)) ||
		    (!by_name && entry->pw_uid == uid)) {
			*result = entry;
			break;
		}
	}
	if (ferror(stream) && error == 0)
		error = EIO;
	fclose(stream);
	return error;
}

int
getpwnam_r(const char *name, struct passwd *entry, char *buffer, size_t size,
	   struct passwd **result)
{
	if (name == NULL || entry == NULL || buffer == NULL || result == NULL)
		return EINVAL;
	return passwd_lookup(name, 0, 1, entry, buffer, size, result);
}

int
getpwuid_r(uid_t uid, struct passwd *entry, char *buffer, size_t size,
	   struct passwd **result)
{
	if (entry == NULL || buffer == NULL || result == NULL)
		return EINVAL;
	return passwd_lookup(NULL, uid, 0, entry, buffer, size, result);
}

struct passwd *
getpwnam(const char *name)
{
	struct passwd *result;
	int error = getpwnam_r(name, &passwd_result, passwd_buffer,
			       sizeof(passwd_buffer), &result);
	if (error != 0)
		errno = error;
	return error == 0 ? result : NULL;
}

struct passwd *
getpwuid(uid_t uid)
{
	struct passwd *result;
	int error = getpwuid_r(uid, &passwd_result, passwd_buffer,
			       sizeof(passwd_buffer), &result);
	if (error != 0)
		errno = error;
	return error == 0 ? result : NULL;
}

void
setpwent(void)
{
	pthread_mutex_lock(&account_lock);
	if (passwd_stream != NULL)
		fclose(passwd_stream);
	passwd_stream = fopen("/etc/passwd", "r");
	pthread_mutex_unlock(&account_lock);
}

struct passwd *
getpwent(void)
{
	char line[ACCOUNT_LINE_MAX];
	struct passwd *result = NULL;
	pthread_mutex_lock(&account_lock);
	if (passwd_stream == NULL)
		passwd_stream = fopen("/etc/passwd", "r");
	while (passwd_stream != NULL &&
	       fgets(line, sizeof(line), passwd_stream))
		if (line[0] != '#' && line[0] != '\n' &&
		    passwd_parse(line, &passwd_result, passwd_buffer,
				 sizeof(passwd_buffer)) == 0) {
			result = &passwd_result;
			break;
		}
	pthread_mutex_unlock(&account_lock);
	return result;
}

void
endpwent(void)
{
	pthread_mutex_lock(&account_lock);
	if (passwd_stream != NULL)
		fclose(passwd_stream);
	passwd_stream = NULL;
	pthread_mutex_unlock(&account_lock);
}

static int
group_parse(const char *line, struct group *entry, char *buffer, size_t size)
{
	char *field[4], *cursor;
	char **members;
	size_t length = strlen(line) + 1U, strings, aligned, slots = 1U;
	uint32_t gid;

	if (length > size)
		return ERANGE;
	memcpy(buffer, line, length);
	if (split_fields(buffer, field, 4) != 4 || parse_id(field[2], &gid))
		return EINVAL;
	for (cursor = field[3]; *cursor != '\0'; cursor++)
		if (*cursor == ',')
			slots++;
	if (field[3][0] == '\0')
		slots = 0;
	strings = length;
	aligned = (strings + sizeof(char *) - 1U) & ~(sizeof(char *) - 1U);
	if (aligned > size || (slots + 1U) > (size - aligned) / sizeof(char *))
		return ERANGE;
	members = (char **)(void *)(buffer + aligned);
	if (slots != 0) {
		unsigned index = 0;
		cursor = field[3];
		while (1) {
			members[index++] = cursor;
			cursor = strchr(cursor, ',');
			if (cursor == NULL)
				break;
			*cursor++ = '\0';
		}
		members[index] = NULL;
	} else
		members[0] = NULL;
	entry->gr_name = field[0];
	entry->gr_passwd = field[1];
	entry->gr_gid = (gid_t)gid;
	entry->gr_mem = members;
	return 0;
}

static int
group_lookup(const char *name, gid_t gid, int by_name, struct group *entry,
	     char *buffer, size_t size, struct group **result)
{
	FILE *stream = fopen("/etc/group", "r");
	char line[ACCOUNT_LINE_MAX];
	int error = 0;
	*result = NULL;
	if (stream == NULL)
		return errno;
	while (fgets(line, sizeof(line), stream)) {
		if (line[0] == '#' || line[0] == '\n')
			continue;
		error = group_parse(line, entry, buffer, size);
		if (error == EINVAL)
			continue;
		if (error != 0)
			break;
		if ((by_name && !strcmp(entry->gr_name, name)) ||
		    (!by_name && entry->gr_gid == gid)) {
			*result = entry;
			break;
		}
	}
	if (ferror(stream) && error == 0)
		error = EIO;
	fclose(stream);
	return error;
}

int
getgrnam_r(const char *n, struct group *g, char *b, size_t z, struct group **r)
{
	if (!n || !g || !b || !r)
		return EINVAL;
	return group_lookup(n, 0, 1, g, b, z, r);
}
int
getgrgid_r(gid_t id, struct group *g, char *b, size_t z, struct group **r)
{
	if (!g || !b || !r)
		return EINVAL;
	return group_lookup(NULL, id, 0, g, b, z, r);
}
struct group *
getgrnam(const char *n)
{
	struct group *r;
	int e = getgrnam_r(n, &group_result, group_buffer, sizeof(group_buffer),
			   &r);
	if (e)
		errno = e;
	return e ? NULL : r;
}
struct group *
getgrgid(gid_t id)
{
	struct group *r;
	int e = getgrgid_r(id, &group_result, group_buffer,
			   sizeof(group_buffer), &r);
	if (e)
		errno = e;
	return e ? NULL : r;
}

void
setgrent(void)
{
	pthread_mutex_lock(&account_lock);
	if (group_stream)
		fclose(group_stream);
	group_stream = fopen("/etc/group", "r");
	pthread_mutex_unlock(&account_lock);
}
struct group *
getgrent(void)
{
	char line[ACCOUNT_LINE_MAX];
	struct group *r = NULL;
	pthread_mutex_lock(&account_lock);
	if (!group_stream)
		group_stream = fopen("/etc/group", "r");
	while (group_stream && fgets(line, sizeof(line), group_stream))
		if (line[0] != '#' && line[0] != '\n' &&
		    group_parse(line, &group_result, group_buffer,
				sizeof(group_buffer)) == 0) {
			r = &group_result;
			break;
		}
	pthread_mutex_unlock(&account_lock);
	return r;
}
void
endgrent(void)
{
	pthread_mutex_lock(&account_lock);
	if (group_stream)
		fclose(group_stream);
	group_stream = NULL;
	pthread_mutex_unlock(&account_lock);
}

int
initgroups(const char *user, gid_t primary)
{
	FILE *stream;
	char line[ACCOUNT_LINE_MAX], buffer[ACCOUNT_RESULT_MAX];
	struct group entry;
	gid_t groups[ACCOUNT_GROUP_MAX];
	size_t count = 1;
	if (!user) {
		errno = EINVAL;
		return -1;
	}
	groups[0] = primary;
	stream = fopen("/etc/group", "r");
	if (!stream)
		return -1;
	while (fgets(line, sizeof(line), stream)) {
		unsigned i;
		int duplicate = 0;
		if (group_parse(line, &entry, buffer, sizeof(buffer)) != 0)
			continue;
		for (i = 0; entry.gr_mem[i]; i++)
			if (!strcmp(entry.gr_mem[i], user))
				break;
		if (!entry.gr_mem[i])
			continue;
		for (i = 0; i < count; i++)
			if (groups[i] == entry.gr_gid)
				duplicate = 1;
		if (!duplicate) {
			if (count == ACCOUNT_GROUP_MAX) {
				fclose(stream);
				errno = E2BIG;
				return -1;
			}
			groups[count++] = entry.gr_gid;
		}
	}
	fclose(stream);
	return setgroups(count, groups);
}

static long
shadow_number(const char *s, unsigned long empty)
{
	char *end;
	long value;
	if (!s || !*s)
		return (long)empty;
	errno = 0;
	value = strtol(s, &end, 10);
	return errno || *end ? -1L : value;
}

static int
shadow_parse(const char *line, struct spwd *entry, char *buffer, size_t size)
{
	char *field[9];
	size_t length = strlen(line) + 1U;
	if (length > size)
		return ERANGE;
	memcpy(buffer, line, length);
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
	return 0;
}

int
getspnam_r(const char *name, struct spwd *entry, char *buffer, size_t size,
	   struct spwd **result)
{
	FILE *stream;
	char line[ACCOUNT_LINE_MAX];
	int error = 0;
	*result = NULL;
	if (!name || !entry || !buffer || !result)
		return EINVAL;
	stream = fopen("/etc/shadow", "r");
	if (!stream)
		return errno;
	while (fgets(line, sizeof(line), stream)) {
		if (line[0] == '#' || line[0] == '\n')
			continue;
		error = shadow_parse(line, entry, buffer, size);
		if (error == EINVAL)
			continue;
		if (error || !strcmp(entry->sp_namp, name)) {
			if (!error)
				*result = entry;
			break;
		}
	}
	if (ferror(stream) && !error)
		error = EIO;
	fclose(stream);
	return error;
}
struct spwd *
getspnam(const char *n)
{
	struct spwd *r;
	int e = getspnam_r(n, &shadow_result, shadow_buffer,
			   sizeof(shadow_buffer), &r);
	if (e)
		errno = e;
	return e ? NULL : r;
}
void
setspent(void)
{
	pthread_mutex_lock(&account_lock);
	if (shadow_stream)
		fclose(shadow_stream);
	shadow_stream = fopen("/etc/shadow", "r");
	pthread_mutex_unlock(&account_lock);
}
struct spwd *
getspent(void)
{
	char line[ACCOUNT_LINE_MAX];
	struct spwd *r = NULL;
	pthread_mutex_lock(&account_lock);
	if (!shadow_stream)
		shadow_stream = fopen("/etc/shadow", "r");
	while (shadow_stream && fgets(line, sizeof(line), shadow_stream))
		if (line[0] != '#' && line[0] != '\n' &&
		    shadow_parse(line, &shadow_result, shadow_buffer,
				 sizeof(shadow_buffer)) == 0) {
			r = &shadow_result;
			break;
		}
	pthread_mutex_unlock(&account_lock);
	return r;
}
void
endspent(void)
{
	pthread_mutex_lock(&account_lock);
	if (shadow_stream)
		fclose(shadow_stream);
	shadow_stream = NULL;
	pthread_mutex_unlock(&account_lock);
}
