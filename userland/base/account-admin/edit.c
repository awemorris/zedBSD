/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * account-admin's pure part (ws089-p026; edit.h).
 *
 * The account files are lines of fields split by ':' (passwd: name,
 * password, user ID, group ID, display name, home, shell; group: name,
 * password, group ID, members split by ','; shadow: name, hash and the
 * ageing fields).  An edit copies the text into an output buffer line by
 * line, changing only the lines it is about; every other byte is kept.
 * The output always ends with a line end when it is not empty.
 */

#include "edit.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/*
 * How many of the operations are Settings' (the first ones), and how many
 * there are in all (root's follow Settings').
 */
#define EDIT_SETTINGS_OPERATIONS	6U
#define EDIT_OPERATIONS			9U

/*
 * The keys of root's form, each a bit in the set of keys an operation
 * takes and in the set a request has given.
 */
enum edit_key {
	EDIT_KEY_NAME,
	EDIT_KEY_COMMENT,
	EDIT_KEY_SHELL,
	EDIT_KEY_UID,
	EDIT_KEY_GROUP,
	EDIT_KEY_GROUPS,
	EDIT_KEY_GROUPS_ADD,
	EDIT_KEY_PASSWORD,
	EDIT_KEY_HOME,
	EDIT_KEY_FORCE,
	EDIT_KEY_COUNT
};

/*
 * A line of a text being walked: where it starts, its length without its
 * end, and where the next one starts.
 */
struct edit_line {
	const char *start;
	size_t length;
	size_t next;
};

/* The words of the operations, in their order: Settings' six, then root's three. */
static const char *const edit_operations[EDIT_OPERATIONS] = {
	"add",
	"remove",
	"reset-password",
	"group-add",
	"group-remove",
	"system-language",
	"user-add",
	"user-modify",
	"user-remove"
};

/* The words of root's keys, in the order of enum edit_key. */
static const char *const edit_keys[EDIT_KEY_COUNT] = {
	"name",
	"comment",
	"shell",
	"uid",
	"group",
	"groups",
	"groups-add",
	"password",
	"home",
	"force"
};

/*
 * The keys each of root's operations takes (user-add, user-modify,
 * user-remove), as bits of enum edit_key: useradd's, usermod's and
 * userdel's options.
 */
static const unsigned edit_keys_taken[EDIT_OPERATIONS - EDIT_SETTINGS_OPERATIONS] = {
	(1U << EDIT_KEY_NAME) | (1U << EDIT_KEY_COMMENT) | (1U << EDIT_KEY_SHELL) | (1U << EDIT_KEY_UID) |
		(1U << EDIT_KEY_GROUP) | (1U << EDIT_KEY_GROUPS) | (1U << EDIT_KEY_HOME),
	(1U << EDIT_KEY_NAME) | (1U << EDIT_KEY_COMMENT) | (1U << EDIT_KEY_SHELL) | (1U << EDIT_KEY_GROUP) |
		(1U << EDIT_KEY_GROUPS) | (1U << EDIT_KEY_GROUPS_ADD) | (1U << EDIT_KEY_PASSWORD),
	(1U << EDIT_KEY_NAME) | (1U << EDIT_KEY_HOME) | (1U << EDIT_KEY_FORCE)
};

/* The languages the login screen may be in (ws158-p004; the compositor's language.c knows them). */
static const char *const edit_languages[] = { "en", "ja" };

/* The words of the reasons, in their order (ADMIN_OK's is "ok"). */
static const char *const edit_reasons[] = {
	"ok",
	"bad-request",
	"not-administrator",
	"bad-password",
	"no-such-user",
	"name-taken",
	"bad-name",
	"weak-password",
	"last-administrator",
	"self",
	"root",
	"busy",
	"home-exists",
	"failed",
	"bad-value",
	"id-taken",
	"no-password",
	"home-failed"
};

static int edit_parse_settings(char **lines, size_t count, struct admin_request *request);
static int edit_parse_root(char **lines, size_t count, struct admin_request *request);
static int edit_key_find(const char *word);
static int edit_root_value(struct admin_request *request, int key, const char *value);
static int edit_root_complete(const struct admin_request *request, unsigned given);
static int edit_next_line(const char *text, size_t length, size_t at, struct edit_line *line);
static int edit_line_field(const char *line, size_t length, unsigned index, const char **field, size_t *field_length);
static int edit_line_named(const struct edit_line *line, const char *name);
static long edit_number(const char *text, size_t length);
static int edit_put(char *output, size_t capacity, size_t *used, const char *bytes, size_t length);
static void edit_highest_id(const char *text, size_t length, long *highest);
static int edit_has_member(const char *members, size_t length, const char *name);
static int edit_members_rebuilt(const char *members, size_t length, const char *name, int member, char *output, size_t capacity, size_t *used);
static int edit_shell_logs_in(const char *shell, size_t length);
static int edit_contains(const char *text, size_t length, const char *word);
static int edit_login_usable(const char *shadow, size_t shadow_length, const char *name, long day);
static void edit_wipe(char *buffer, size_t size);

/*
 * Reads a request's text (at most ADMIN_REQUEST_MAX bytes, lines ended by
 * line ends, a NUL after its last byte) into a request, its lines cut in
 * place: Settings' form when by_root is 0, root's form otherwise.  Returns
 * ADMIN_OK, ADMIN_BAD_REQUEST, or ADMIN_BAD_VALUE for a value of root's
 * form outside its rules.
 */
int
admin_parse(
	char *text,
	size_t length,
	int by_root,
	struct admin_request *request)
{
	char *lines[ADMIN_LINES_MAX];
	const char *nul;
	size_t count;
	size_t at;
	int reason;

	/* Nothing yet: no user ID chosen, and a new user gets a home. */
	memset(request, 0, sizeof(request[0]));
	request->by_root = by_root;
	request->uid = -1;
	request->make_home = 1;

	/* A text too long, or holding a NUL, is not a request. */
	nul = memchr(text, '\0', length);
	if (length > ADMIN_REQUEST_MAX || nul != NULL)
		return ADMIN_BAD_REQUEST;

	/* The lines, each cut at its end (the last may have none; an empty line after the last end is not one). */
	count = 0;
	at = 0;
	while (at < length) {
		/* Too many lines. */
		if (count == ADMIN_LINES_MAX)
			return ADMIN_BAD_REQUEST;

		/* One line. */
		lines[count] = text + at;
		count++;
		while (at < length && text[at] != '\n')
			at++;

		/* Its end becomes a NUL. */
		if (at < length) {
			text[at] = '\0';
			at++;
		}
	}

	/* Root's form, or Settings'. */
	if (by_root) {
		reason = edit_parse_root(lines, count, request);
	} else {
		reason = edit_parse_settings(lines, count, request);
	}

	/* Reports a request not understood. */
	if (reason != ADMIN_OK)
		return reason;

	/* Succeeded: the request is read. */
	return ADMIN_OK;
}

/*
 * Reports a reason's word for the answer line.
 */
const char *
admin_reason_word(
	int reason)
{
	/* A reason not known is a failure. */
	if (reason < 0 || (size_t)reason >= sizeof(edit_reasons) / sizeof(edit_reasons[0]))
		return "failed";

	/* Its word. */
	return edit_reasons[reason];
}

/*
 * Reports an operation's word.
 */
const char *
admin_operation_word(
	enum admin_operation operation)
{
	/* An operation not known. */
	if ((size_t)operation >= sizeof(edit_operations) / sizeof(edit_operations[0]))
		return "unknown";

	/* Its word. */
	return edit_operations[operation];
}

/*
 * Reports whether a language is one the login screen may be in ("en",
 * "ja"): 1 when it is, 0 otherwise.
 */
int
admin_language_valid(
	const char *language)
{
	size_t i;
	int same;

	/* Each language known. */
	for (i = 0; i < sizeof(edit_languages) / sizeof(edit_languages[0]); i++) {
		same = strcmp(language, edit_languages[i]);
		if (same == 0)
			return 1;
	}

	/* Reports a language the login screen does not have. */
	return 0;
}

/*
 * Reports whether a new user's name is allowed: a lower-case letter, then
 * up to 31 lower-case letters, digits, '-' or '_'.
 */
int
admin_name_valid(
	const char *name)
{
	size_t i;
	char c;

	/* The first character, a lower-case letter. */
	if (name[0] < 'a' || name[0] > 'z')
		return 0;

	/* The rest, and the length. */
	for (i = 1; name[i] != '\0'; i++) {
		/* Too long. */
		if (i >= ADMIN_NAME_MAX)
			return 0;

		/* A character allowed. */
		c = name[i];
		if ((c >= 'a' && c <= 'z') ||
		    (c >= '0' && c <= '9') ||
		    c == '-' ||
		    c == '_')
			continue;
		return 0;
	}

	/* Allowed. */
	return 1;
}

/*
 * Reports whether a display name is allowed: printable, without ':', not
 * longer than 64 bytes (UTF-8 is taken as it is).
 */
int
admin_display_valid(
	const char *display)
{
	size_t length;
	size_t i;

	/* Not too long. */
	length = strlen(display);
	if (length > 64U)
		return 0;

	/* No ':', and no control character. */
	for (i = 0; i < length; i++) {
		/* One byte. */
		if (display[i] == ':' ||
		    (unsigned char)display[i] < 0x20U ||
		    display[i] == 0x7f)
			return 0;
	}

	/* Allowed. */
	return 1;
}

/*
 * Reports whether a shell may be given to a user: an absolute path, not
 * longer than ADMIN_SHELL_MAX bytes, without ':' or a control character.
 * Whether the file is there is main.c's to check.
 */
int
admin_shell_valid(
	const char *shell)
{
	size_t length;
	size_t i;

	/* An absolute path. */
	if (shell[0] != '/')
		return 0;

	/* Not too long. */
	length = strlen(shell);
	if (length > ADMIN_SHELL_MAX)
		return 0;

	/* No ':', which would break the passwd line, and no control character. */
	for (i = 0; i < length; i++) {
		/* One byte. */
		if (shell[i] == ':')
			return 0;
		if ((unsigned char)shell[i] < 0x20U || shell[i] == 0x7f)
			return 0;
	}

	/* Allowed. */
	return 1;
}

/*
 * Reports whether a list of groups (split by ',') names only the groups a
 * person may be put in -- wheel and network, as in Settings -- each one
 * not empty.  An empty list is allowed when may_be_empty is nonzero (it
 * takes the user out of every group).
 */
int
admin_groups_valid(
	const char *groups,
	int may_be_empty)
{
	const char *comma;
	size_t length;
	size_t at;
	size_t item;
	int wheel;
	int network;

	/* The empty list. */
	length = strlen(groups);
	if (length == 0U)
		return may_be_empty;

	/* Each group of the list. */
	at = 0;
	for (;;) {
		/* One group, to the next ',' or the list's end. */
		comma = memchr(groups + at, ',', length - at);
		item = length - at;
		if (comma != NULL)
			item = (size_t)(comma - (groups + at));

		/* wheel or network, and no other. */
		wheel = 1;
		network = 1;
		if (item == 5U)
			wheel = memcmp(groups + at, "wheel", 5U);
		if (item == 7U)
			network = memcmp(groups + at, "network", 7U);
		if (wheel != 0 && network != 0)
			return 0;

		/* The list's end. */
		if (comma == NULL)
			break;
		at += item + 1U;
	}

	/* Allowed. */
	return 1;
}

/*
 * Finds a field (counted from 0) of the line of a text whose first field
 * is a name.  Returns 0, or ENOENT when there is no such line or field.
 */
int
admin_field(
	const char *text,
	size_t length,
	const char *name,
	unsigned index,
	const char **field,
	size_t *field_length)
{
	struct edit_line line;
	size_t at;
	int more;
	int named;
	int found;

	/* Each line, until the name's. */
	at = 0;
	for (;;) {
		/* The next line, or the end. */
		more = edit_next_line(text, length, at, &line);
		if (!more)
			return ENOENT;
		at = line.next;

		/* The name's line: its field. */
		named = edit_line_named(&line, name);
		if (!named)
			continue;
		found = edit_line_field(line.start, line.length, index, field, field_length);
		if (!found)
			return ENOENT;

		/* Succeeded: the field. */
		return 0;
	}
}

/*
 * Reports a number field of the line of a name, or -1 when there is no
 * such line, field, or number.
 */
long
admin_number_field(
	const char *text,
	size_t length,
	const char *name,
	unsigned index)
{
	const char *field;
	size_t field_length;
	long number;
	int error;

	/* The field. */
	error = admin_field(text, length, name, index, &field, &field_length);
	if (error != 0)
		return -1;

	/* Its number, or -1 when the field is not one. */
	number = edit_number(field, field_length);

	/* Reports the number. */
	return number;
}

/*
 * Reports whether any line of an account or group file has a number in a
 * field (counted from 0): 1 when one has, 0 otherwise.
 */
int
admin_number_used(
	const char *text,
	size_t length,
	unsigned index,
	long number)
{
	struct edit_line line;
	const char *field;
	size_t field_length;
	size_t at;
	long found_number;
	int more;
	int found;

	/* Each line's field. */
	at = 0;
	for (;;) {
		/* The next line, or the end. */
		more = edit_next_line(text, length, at, &line);
		if (!more)
			break;
		at = line.next;

		/* The line's number, when it has the field. */
		found = edit_line_field(line.start, line.length, index, &field, &field_length);
		found_number = -1;
		if (found)
			found_number = edit_number(field, field_length);

		/* This line uses the number. */
		if (found_number == number)
			return 1;
	}

	/* Succeeded: no line uses it. */
	return 0;
}

/*
 * Reports the user ID a new person gets: one more than the highest of the
 * floor (what main.c found used outside the account files: the record of
 * the last ID given, the homes' owners, the job spools) and of every user
 * ID and group ID from ADMIN_ID_FIRST to ADMIN_ID_LAST in the files, so
 * that an ID once given is never given again while anything of its last
 * owner may be left.  Returns -1 when the range is used up.
 */
long
admin_next_id(
	const char *passwd,
	size_t passwd_length,
	const char *group,
	size_t group_length,
	long floor)
{
	long highest;

	/* Below the first person's ID when nothing is used yet. */
	highest = ADMIN_ID_FIRST - 1;
	if (floor > highest)
		highest = floor;

	/* The highest user ID, and the highest group ID. */
	edit_highest_id(passwd, passwd_length, &highest);
	edit_highest_id(group, group_length, &highest);

	/* The range is used up. */
	if (highest >= ADMIN_ID_LAST)
		return -1;

	/* Succeeded: the next number. */
	return highest + 1;
}

/*
 * Copies a text with one field (counted from 0) of the lines of a name
 * replaced by a value; every other byte is kept.  Returns 0, ENOENT when
 * no line names it or the line has no such field, EINVAL for a value that
 * would break the line's fields, or ERANGE when the output is too small.
 */
int
admin_field_set(
	const char *text,
	size_t length,
	const char *name,
	unsigned index,
	const char *value,
	char *output,
	size_t capacity,
	size_t *written)
{
	struct edit_line line;
	const char *field;
	size_t field_length;
	size_t value_length;
	size_t clean_length;
	size_t prefix;
	size_t at;
	size_t used;
	int changed;
	int more;
	int named;
	int found;
	int error;

	/* A value with a ':' or a line end would break the line. */
	value_length = strlen(value);
	clean_length = strcspn(value, ":\n");
	if (clean_length != value_length)
		return EINVAL;

	/* Each line: the name's with its field replaced, the others kept. */
	at = 0;
	used = 0;
	changed = 0;
	for (;;) {
		/* The next line, or the end. */
		more = edit_next_line(text, length, at, &line);
		if (!more)
			break;
		at = line.next;

		/* Another name's line: kept with its end. */
		named = edit_line_named(&line, name);
		if (!named) {
			error = edit_put(output, capacity, &used, line.start, line.length);
			if (error == 0)
				error = edit_put(output, capacity, &used, "\n", 1U);
			if (error != 0)
				return error;
			continue;
		}

		/* The name's line without the field. */
		found = edit_line_field(line.start, line.length, index, &field, &field_length);
		if (!found)
			return ENOENT;

		/* The fields before it, the value, and the rest of the line. */
		prefix = (size_t)(field - line.start);
		error = edit_put(output, capacity, &used, line.start, prefix);
		if (error == 0)
			error = edit_put(output, capacity, &used, value, value_length);
		if (error == 0)
			error = edit_put(output, capacity, &used, field + field_length, line.length - prefix - field_length);
		if (error == 0)
			error = edit_put(output, capacity, &used, "\n", 1U);
		if (error != 0)
			return error;
		changed = 1;
	}

	/* No line named it. */
	*written = used;
	if (!changed)
		return ENOENT;

	/* Succeeded: the text with the field replaced. */
	return 0;
}

/*
 * Copies a shadow file's text with a user's password locked (a '!' put
 * before its hash, unless one is there) or unlocked (one '!' taken off).
 * An unlock that would leave no password that can be typed -- nothing
 * after the '!', or another '!' or a '*' -- is refused, since an empty
 * hash takes an empty password at login.  Returns 0 (the copy, changed or
 * not), ENOENT when the shadow has no line of the user, EACCES for the
 * refused unlock, or ERANGE.
 */
int
admin_lock_hash(
	const char *shadow,
	size_t length,
	const char *name,
	enum admin_lock lock,
	char *output,
	size_t capacity,
	size_t *written)
{
	char changed[ADMIN_HASH_FIELD_MAX + 2U];
	const char *hash;
	const char *rest;
	size_t hash_length;
	size_t rest_length;
	int locked;
	int error;

	/* The user's hash. */
	error = admin_field(shadow, length, name, 1U, &hash, &hash_length);
	if (error != 0)
		return ENOENT;
	if (hash_length > ADMIN_HASH_FIELD_MAX)
		return ERANGE;

	/* As it is, unless the lock changes it. */
	memcpy(changed, hash, hash_length);
	changed[hash_length] = '\0';

	/* Whether a '!' locks it already. */
	locked = 0;
	if (hash_length > 0U && hash[0] == '!')
		locked = 1;

	/* A lock puts a '!' before a hash that has none. */
	if (lock == ADMIN_LOCK_SET && !locked) {
		changed[0] = '!';
		memcpy(changed + 1, hash, hash_length);
		changed[hash_length + 1U] = '\0';
	}

	/* An unlock takes the '!' off, when there is a password behind it. */
	if (lock == ADMIN_LOCK_CLEAR && locked) {
		rest = hash + 1;
		rest_length = hash_length - 1U;

		/* Nothing, or no hash, behind the '!'. */
		if (rest_length == 0U ||
		    rest[0] == '!' ||
		    rest[0] == '*') {
			edit_wipe(changed, sizeof(changed));
			return EACCES;
		}

		/* The hash without its '!'. */
		memcpy(changed, rest, rest_length);
		changed[rest_length] = '\0';
	}

	/* The line with the hash. */
	error = admin_field_set(shadow, length, name, 1U, changed, output, capacity, written);
	edit_wipe(changed, sizeof(changed));
	if (error != 0)
		return error;

	/* Succeeded: the copy with the password locked or not. */
	return 0;
}

/*
 * Copies a text without the lines whose first field is a name.  Returns 0,
 * ENOENT when there was none, or ERANGE when the output is too small.
 */
int
admin_line_remove(
	const char *text,
	size_t length,
	const char *name,
	char *output,
	size_t capacity,
	size_t *written)
{
	struct edit_line line;
	size_t at;
	size_t used;
	int removed;
	int more;
	int named;
	int error;

	/* Each line, kept unless it is the name's. */
	at = 0;
	used = 0;
	removed = 0;
	for (;;) {
		/* The next line, or the end. */
		more = edit_next_line(text, length, at, &line);
		if (!more)
			break;
		at = line.next;

		/* The name's goes. */
		named = edit_line_named(&line, name);
		if (named) {
			removed = 1;
			continue;
		}

		/* Another, kept with its end. */
		error = edit_put(output, capacity, &used, line.start, line.length);
		if (error == 0)
			error = edit_put(output, capacity, &used, "\n", 1U);
		if (error != 0)
			return error;
	}

	/* None was the name's. */
	*written = used;
	if (!removed)
		return ENOENT;

	/* Succeeded: the text without it. */
	return 0;
}

/*
 * Copies a text with a line (without its end) added at its end.  Returns
 * 0, or ERANGE when the output is too small.
 */
int
admin_line_append(
	const char *text,
	size_t length,
	const char *line,
	char *output,
	size_t capacity,
	size_t *written)
{
	size_t line_length;
	size_t used;
	int error;

	/* The text, ended. */
	used = 0;
	error = edit_put(output, capacity, &used, text, length);
	if (error == 0 &&
	    length > 0U &&
	    text[length - 1U] != '\n')
		error = edit_put(output, capacity, &used, "\n", 1U);

	/* The line, ended. */
	line_length = strlen(line);
	if (error == 0)
		error = edit_put(output, capacity, &used, line, line_length);
	if (error == 0)
		error = edit_put(output, capacity, &used, "\n", 1U);
	if (error != 0)
		return error;

	/* Succeeded: the text with the line. */
	*written = used;
	return 0;
}

/*
 * Copies a group file's text with a name made a member of a group, or
 * not one (member 0), in the group's list of members; the group's other
 * fields are kept, and a primary group is never changed here.  Returns 0,
 * ENOENT when there is no such group, or ERANGE.
 */
int
admin_member_set(
	const char *text,
	size_t length,
	const char *group,
	const char *name,
	int member,
	char *output,
	size_t capacity,
	size_t *written)
{
	struct edit_line line;
	const char *members;
	size_t members_length;
	size_t prefix;
	size_t at;
	size_t used;
	int found;
	int more;
	int named;
	int has;
	int error;

	/* Each line: the group's rebuilt, the others kept. */
	at = 0;
	used = 0;
	found = 0;
	for (;;) {
		/* The next line, or the end. */
		more = edit_next_line(text, length, at, &line);
		if (!more)
			break;
		at = line.next;

		/* Another group's: kept. */
		named = edit_line_named(&line, group);
		has = 0;
		if (named)
			has = edit_line_field(line.start, line.length, 3U, &members, &members_length);
		if (!named || !has) {
			error = edit_put(output, capacity, &used, line.start, line.length);
			if (error == 0)
				error = edit_put(output, capacity, &used, "\n", 1U);
			if (error != 0)
				return error;
			continue;
		}

		/* The group's: its first three fields as they are, then its members with the name or without it. */
		found = 1;
		prefix = (size_t)(members - line.start);
		error = edit_put(output, capacity, &used, line.start, prefix);
		if (error == 0)
			error = edit_members_rebuilt(members, members_length, name, member, output, capacity, &used);
		if (error == 0)
			error = edit_put(output, capacity, &used, members + members_length, line.length - prefix - members_length);
		if (error == 0)
			error = edit_put(output, capacity, &used, "\n", 1U);
		if (error != 0)
			return error;
	}

	/* No such group. */
	*written = used;
	if (!found)
		return ENOENT;

	/* Succeeded: the text with the group changed. */
	return 0;
}

/*
 * Copies a group file's text with a name taken out of every group's list
 * of members.  Returns 0, or ERANGE.
 */
int
admin_member_forget(
	const char *text,
	size_t length,
	const char *name,
	char *output,
	size_t capacity,
	size_t *written)
{
	struct edit_line line;
	const char *members;
	size_t members_length;
	size_t prefix;
	size_t at;
	size_t used;
	int more;
	int has;
	int error;

	/* Each line: its members without the name. */
	at = 0;
	used = 0;
	for (;;) {
		/* The next line, or the end. */
		more = edit_next_line(text, length, at, &line);
		if (!more)
			break;
		at = line.next;

		/* A line without a members field is kept. */
		has = edit_line_field(line.start, line.length, 3U, &members, &members_length);
		if (!has) {
			error = edit_put(output, capacity, &used, line.start, line.length);
			if (error == 0)
				error = edit_put(output, capacity, &used, "\n", 1U);
			if (error != 0)
				return error;
			continue;
		}

		/* The line with its members rebuilt. */
		prefix = (size_t)(members - line.start);
		error = edit_put(output, capacity, &used, line.start, prefix);
		if (error == 0)
			error = edit_members_rebuilt(members, members_length, name, 0, output, capacity, &used);
		if (error == 0)
			error = edit_put(output, capacity, &used, members + members_length, line.length - prefix - members_length);
		if (error == 0)
			error = edit_put(output, capacity, &used, "\n", 1U);
		if (error != 0)
			return error;
	}

	/* Succeeded: the text without the name in any list. */
	*written = used;
	return 0;
}

/*
 * Reports whether a user is in a group: its primary group ID is the
 * group's, or the group names the user.
 */
int
admin_in_group(
	const char *group_text,
	size_t group_length,
	const char *group,
	const char *name,
	long primary)
{
	const char *members;
	size_t members_length;
	long gid;
	int error;
	int has;

	/* The group's ID: the user's primary group. */
	gid = admin_number_field(group_text, group_length, group, 2U);
	if (gid >= 0 && gid == primary)
		return 1;

	/* Its members: the user named. */
	error = admin_field(group_text, group_length, group, 3U, &members, &members_length);
	if (error != 0)
		return 0;
	has = edit_has_member(members, members_length, name);

	/* Named or not. */
	return has;
}

/*
 * Reports whether a group named after a user is the user's own, which
 * goes with it: its ID is a person's (ADMIN_ID_FIRST to ADMIN_ID_LAST) and,
 * when the user is known (uid not -1), the user's ID or primary group;
 * nobody in passwd (the text after the user's removal) has it as a
 * primary group; and its list of members is empty or the user alone.
 */
int
admin_group_removable(
	const char *group,
	size_t group_length,
	const char *passwd,
	size_t passwd_length,
	const char *name,
	long uid,
	long gid)
{
	const char *members;
	size_t members_length;
	size_t name_length;
	long group_id;
	int used;
	int error;
	int same;

	/* A person's group. */
	group_id = admin_number_field(group, group_length, name, 2U);
	if (group_id < ADMIN_ID_FIRST || group_id > ADMIN_ID_LAST)
		return 0;

	/* The user's own: its ID, or its primary group. */
	if (uid >= 0 &&
	    group_id != uid &&
	    group_id != gid)
		return 0;

	/* Somebody else's primary group stays. */
	used = admin_number_used(passwd, passwd_length, 3U, group_id);
	if (used)
		return 0;

	/* A group without a members field has no members. */
	error = admin_field(group, group_length, name, 3U, &members, &members_length);
	if (error != 0)
		return 1;

	/* No member, or the user alone. */
	if (members_length == 0U)
		return 1;
	name_length = strlen(name);
	if (members_length != name_length)
		return 0;
	same = memcmp(members, name, name_length);
	if (same != 0)
		return 0;

	/* Succeeded: the group is the user's own. */
	return 1;
}

/*
 * Counts the administrators who can log in among the people's accounts:
 * a user ID from ADMIN_ID_FIRST to ADMIN_ID_LAST, in wheel, a shell that
 * logs in (as the desktop's list of users judges it), and a shadow line
 * with a password that can be typed and an account that has not expired
 * on the day (days since 1970), as passkey judges it.
 */
size_t
admin_administrators(
	const char *passwd,
	size_t passwd_length,
	const char *group,
	size_t group_length,
	const char *shadow,
	size_t shadow_length,
	long day)
{
	struct edit_line line;
	const char *field;
	size_t field_length;
	char name[ADMIN_NAME_MAX + 1U];
	size_t count;
	size_t at;
	long uid;
	long gid;
	int more;
	int found;
	int logs_in;
	int usable;
	int in;

	/* Each account. */
	at = 0;
	count = 0;
	for (;;) {
		/* The next line, or the end. */
		more = edit_next_line(passwd, passwd_length, at, &line);
		if (!more)
			break;
		at = line.next;

		/* Its name, which fits. */
		found = edit_line_field(line.start, line.length, 0U, &field, &field_length);
		if (!found ||
		    field_length == 0U ||
		    field_length > ADMIN_NAME_MAX)
			continue;
		memcpy(name, field, field_length);
		name[field_length] = '\0';

		/* A person's account. */
		found = edit_line_field(line.start, line.length, 2U, &field, &field_length);
		uid = -1;
		if (found)
			uid = edit_number(field, field_length);
		if (uid < ADMIN_ID_FIRST || uid > ADMIN_ID_LAST)
			continue;

		/* A shell that logs in (no shell field is the default shell). */
		found = edit_line_field(line.start, line.length, 6U, &field, &field_length);
		logs_in = 1;
		if (found)
			logs_in = edit_shell_logs_in(field, field_length);
		if (!logs_in)
			continue;

		/* In wheel, by its primary group or by name. */
		found = edit_line_field(line.start, line.length, 3U, &field, &field_length);
		gid = -1;
		if (found)
			gid = edit_number(field, field_length);
		in = admin_in_group(group, group_length, "wheel", name, gid);
		if (!in)
			continue;

		/* A password that can be typed, on an account not expired. */
		usable = edit_login_usable(shadow, shadow_length, name, day);
		if (usable)
			count++;
	}

	/* The count. */
	return count;
}

/*
 * Reads Settings' form: the caller's password, the operation (one of
 * Settings' six) and its arguments, one a line.
 */
static int
edit_parse_settings(
	char **lines,
	size_t count,
	struct admin_request *request)
{
	size_t i;
	size_t arguments;
	int known;
	int same;

	/* The password and the operation at least. */
	if (count < 2U || lines[0][0] == '\0')
		return ADMIN_BAD_REQUEST;

	/* The operation's word, one of Settings'. */
	request->password = lines[0];
	for (i = 0; i < EDIT_SETTINGS_OPERATIONS; i++) {
		/* This one. */
		same = strcmp(lines[1], edit_operations[i]);
		if (same == 0)
			break;
	}

	/* A word not known. */
	if (i == EDIT_SETTINGS_OPERATIONS)
		return ADMIN_BAD_REQUEST;
	request->operation = (enum admin_operation)i;

	/* Its arguments: four for an addition, one for the system language, two for the rest. */
	arguments = 2U;
	if (request->operation == ADMIN_ADD)
		arguments = 4U;
	if (request->operation == ADMIN_SYSTEM_LANGUAGE)
		arguments = 1U;
	if (count != 2U + arguments)
		return ADMIN_BAD_REQUEST;
	request->name = lines[2];

	/* Each operation's own. */
	switch (request->operation) {
	case ADMIN_ADD:
		/* The display name, the new password, and admin or user. */
		request->display = lines[3];
		request->fresh = lines[4];
		same = strcmp(lines[5], "admin");
		if (same == 0) {
			request->administrator = 1;
			break;
		}

		/* Or a plain user. */
		same = strcmp(lines[5], "user");
		if (same != 0)
			return ADMIN_BAD_REQUEST;
		break;
	case ADMIN_REMOVE:
		/* keep-home or remove-home. */
		same = strcmp(lines[3], "remove-home");
		if (same == 0) {
			request->remove_home = 1;
			break;
		}

		/* Or keep it. */
		same = strcmp(lines[3], "keep-home");
		if (same != 0)
			return ADMIN_BAD_REQUEST;
		break;
	case ADMIN_RESET_PASSWORD:
		request->fresh = lines[3];
		break;
	case ADMIN_SYSTEM_LANGUAGE:
		/* A language the login screen knows. */
		known = admin_language_valid(lines[2]);
		if (!known)
			return ADMIN_BAD_REQUEST;
		break;
	case ADMIN_GROUP_ADD:
	case ADMIN_GROUP_REMOVE:
		/* wheel or network, and no other group. */
		request->group = lines[3];
		same = strcmp(lines[3], "wheel");
		if (same == 0)
			break;
		same = strcmp(lines[3], "network");
		if (same != 0)
			return ADMIN_BAD_REQUEST;
		break;
	default:
		return ADMIN_BAD_REQUEST;
	}

	/* Succeeded: Settings' request is read. */
	return ADMIN_OK;
}

/*
 * Reads root's form: the operation (one of root's three), then one line a
 * key, "KEY VALUE" (cut at the first space; the value may be empty), each
 * key at most once and only those the operation takes.
 */
static int
edit_parse_root(
	char **lines,
	size_t count,
	struct admin_request *request)
{
	unsigned given;
	unsigned taken;
	unsigned bit;
	size_t operation;
	size_t i;
	char *space;
	int reason;
	int key;
	int same;

	/* The operation at least. */
	if (count < 1U)
		return ADMIN_BAD_REQUEST;

	/* The operation's word, one of root's. */
	for (operation = EDIT_SETTINGS_OPERATIONS; operation < EDIT_OPERATIONS; operation++) {
		/* This one. */
		same = strcmp(lines[0], edit_operations[operation]);
		if (same == 0)
			break;
	}

	/* A word not known. */
	if (operation == EDIT_OPERATIONS)
		return ADMIN_BAD_REQUEST;
	request->operation = (enum admin_operation)operation;
	taken = edit_keys_taken[operation - EDIT_SETTINGS_OPERATIONS];

	/* Each key's line. */
	given = 0;
	for (i = 1; i < count; i++) {
		/* The key, cut at the first space; a line without one is not a key's. */
		space = strchr(lines[i], ' ');
		if (space == NULL)
			return ADMIN_BAD_REQUEST;
		*space = '\0';

		/* A key known, taken by the operation, and not given before. */
		key = edit_key_find(lines[i]);
		if (key < 0)
			return ADMIN_BAD_REQUEST;
		bit = 1U << key;
		if ((taken & bit) == 0U)
			return ADMIN_BAD_REQUEST;
		if ((given & bit) != 0U)
			return ADMIN_BAD_REQUEST;
		given |= bit;

		/* Its value, by the key's rules. */
		reason = edit_root_value(request, key, space + 1);
		if (reason != ADMIN_OK)
			return reason;
	}

	/* The rules across the keys. */
	reason = edit_root_complete(request, given);
	if (reason != ADMIN_OK)
		return reason;

	/* Succeeded: root's request is read. */
	return ADMIN_OK;
}

/*
 * Reports the key a word names (enum edit_key), or -1 for none.
 */
static int
edit_key_find(
	const char *word)
{
	int key;
	int same;

	/* Each key's word. */
	for (key = 0; key < EDIT_KEY_COUNT; key++) {
		same = strcmp(word, edit_keys[key]);
		if (same == 0)
			return key;
	}

	/* No key has the word. */
	return -1;
}

/*
 * Takes one key's value of root's form into the request.  Returns
 * ADMIN_OK, or ADMIN_BAD_VALUE for a value outside the key's rules.
 */
static int
edit_root_value(
	struct admin_request *request,
	int key,
	const char *value)
{
	size_t value_length;
	long number;
	int valid;
	int same;

	/* Each key's rule. */
	switch (key) {
	case EDIT_KEY_NAME:
		/* Checked by the operation: a new name by the rules, an old one by being there. */
		request->name = value;
		break;
	case EDIT_KEY_COMMENT:
		/* The display name, by its rules. */
		valid = admin_display_valid(value);
		if (!valid)
			return ADMIN_BAD_VALUE;
		request->display = value;
		break;
	case EDIT_KEY_SHELL:
		/* An absolute path that cannot break the passwd line. */
		valid = admin_shell_valid(value);
		if (!valid)
			return ADMIN_BAD_VALUE;
		request->shell = value;
		break;
	case EDIT_KEY_UID:
		/* A person's user ID. */
		value_length = strlen(value);
		number = edit_number(value, value_length);
		if (number < ADMIN_ID_FIRST || number > ADMIN_ID_LAST)
			return ADMIN_BAD_VALUE;
		request->uid = number;
		break;
	case EDIT_KEY_GROUP:
		/* Checked once the name is known. */
		request->primary = value;
		break;
	case EDIT_KEY_GROUPS:
		/* The groups that replace the user's (none takes it out of all). */
		valid = admin_groups_valid(value, 1);
		if (!valid)
			return ADMIN_BAD_VALUE;
		request->groups = value;
		request->groups_append = 0;
		break;
	case EDIT_KEY_GROUPS_ADD:
		/* The groups the user is added to. */
		valid = admin_groups_valid(value, 0);
		if (!valid)
			return ADMIN_BAD_VALUE;
		request->groups = value;
		request->groups_append = 1;
		break;
	case EDIT_KEY_PASSWORD:
		/* lock or unlock. */
		same = strcmp(value, "lock");
		if (same == 0) {
			request->lock = ADMIN_LOCK_SET;
			break;
		}

		/* Or unlock. */
		same = strcmp(value, "unlock");
		if (same != 0)
			return ADMIN_BAD_VALUE;
		request->lock = ADMIN_LOCK_CLEAR;
		break;
	case EDIT_KEY_HOME:
		/* An addition's make or none, a removal's remove or keep. */
		if (request->operation == ADMIN_USER_ADD) {
			/* A home made with the skeleton, or none. */
			same = strcmp(value, "make");
			if (same == 0)
				break;
			same = strcmp(value, "none");
			if (same != 0)
				return ADMIN_BAD_VALUE;
			request->make_home = 0;
			break;
		}

		/* The home removed, or kept. */
		same = strcmp(value, "keep");
		if (same == 0)
			break;
		same = strcmp(value, "remove");
		if (same != 0)
			return ADMIN_BAD_VALUE;
		request->remove_home = 1;
		break;
	case EDIT_KEY_FORCE:
		/* A busy user removed all the same. */
		same = strcmp(value, "yes");
		if (same != 0)
			return ADMIN_BAD_VALUE;
		request->force = 1;
		break;
	default:
		return ADMIN_BAD_REQUEST;
	}

	/* Succeeded: the value is taken. */
	return ADMIN_OK;
}

/*
 * Checks the rules across the keys of root's form: a name; groups either
 * replaced or added to; a modification that changes something; a home not
 * removed under a busy user's running programs; a primary group that is
 * wheel, network or the user's own.
 */
static int
edit_root_complete(
	const struct admin_request *request,
	unsigned given)
{
	unsigned changes;
	int wheel;
	int network;
	int own;

	/* A name. */
	if (request->name == NULL || request->name[0] == '\0')
		return ADMIN_BAD_REQUEST;

	/* The groups replaced, or added to, not both. */
	if ((given & (1U << EDIT_KEY_GROUPS)) != 0U && (given & (1U << EDIT_KEY_GROUPS_ADD)) != 0U)
		return ADMIN_BAD_REQUEST;

	/* A modification changes something. */
	changes = given & ~(1U << EDIT_KEY_NAME);
	if (request->operation == ADMIN_USER_MODIFY && changes == 0U)
		return ADMIN_BAD_REQUEST;

	/* A home is not removed while the user's programs may still write in it. */
	if (request->remove_home && request->force)
		return ADMIN_BAD_REQUEST;

	/* No primary group asked for. */
	if (request->primary == NULL)
		return ADMIN_OK;

	/* wheel, network, or the user's own group. */
	wheel = strcmp(request->primary, "wheel");
	network = strcmp(request->primary, "network");
	own = strcmp(request->primary, request->name);
	if (wheel != 0 &&
	    network != 0 &&
	    own != 0)
		return ADMIN_BAD_VALUE;

	/* Succeeded: the request holds together. */
	return ADMIN_OK;
}

/*
 * Finds the line of a text that starts at an offset.  Returns 1 with it,
 * 0 at the text's end.
 */
static int
edit_next_line(
	const char *text,
	size_t length,
	size_t at,
	struct edit_line *line)
{
	const char *end;

	/* The text's end. */
	if (at >= length)
		return 0;

	/* To the line's end, or the text's. */
	line->start = text + at;
	end = memchr(line->start, '\n', length - at);
	line->length = length - at;
	if (end != NULL)
		line->length = (size_t)(end - line->start);
	line->next = at + line->length + 1U;

	/* Succeeded: the line. */
	return 1;
}

/*
 * Finds a field (counted from 0) of one line.  Returns 1 with it, 0 when
 * the line has fewer fields.
 */
static int
edit_line_field(
	const char *line,
	size_t length,
	unsigned index,
	const char **field,
	size_t *field_length)
{
	const char *colon;
	size_t at;
	unsigned i;

	/* Past the fields before it. */
	at = 0;
	for (i = 0; i < index; i++) {
		/* The next ':'. */
		colon = memchr(line + at, ':', length - at);
		if (colon == NULL)
			return 0;
		at = (size_t)(colon - line) + 1U;
	}

	/* The field, to the next ':' or the line's end. */
	*field = line + at;
	colon = memchr(line + at, ':', length - at);
	*field_length = length - at;
	if (colon != NULL)
		*field_length = (size_t)(colon - (line + at));

	/* Succeeded: the field. */
	return 1;
}

/*
 * Reports whether a line's first field is a name.
 */
static int
edit_line_named(
	const struct edit_line *line,
	const char *name)
{
	const char *field;
	size_t field_length;
	size_t length;
	int found;
	int same;

	/* The first field against the name. */
	found = edit_line_field(line->start, line->length, 0U, &field, &field_length);
	length = strlen(name);
	if (!found || field_length != length)
		return 0;
	same = memcmp(field, name, length);
	if (same != 0)
		return 0;

	/* The name's line. */
	return 1;
}

/*
 * Reports a field's decimal number, or -1 when it is not one.
 */
static long
edit_number(
	const char *text,
	size_t length)
{
	long value;
	size_t i;

	/* Digits only, not too many. */
	if (length == 0U || length > 9U)
		return -1;
	value = 0;
	for (i = 0; i < length; i++) {
		/* One digit. */
		if (text[i] < '0' || text[i] > '9')
			return -1;
		value = value * 10 + (text[i] - '0');
	}

	/* The number. */
	return value;
}

/*
 * Appends bytes to an output; returns 0, or ERANGE when they do not fit.
 */
static int
edit_put(
	char *output,
	size_t capacity,
	size_t *used,
	const char *bytes,
	size_t length)
{
	/* Room for them. */
	if (length > capacity - *used)
		return ERANGE;

	/* The bytes. */
	memcpy(output + *used, bytes, length);
	*used += length;

	/* Succeeded: the bytes are in the output. */
	return 0;
}

/*
 * Raises a highest ID to the largest number from ADMIN_ID_FIRST to
 * ADMIN_ID_LAST that a line of an account or group file has as its ID
 * (its third field).
 */
static void
edit_highest_id(
	const char *text,
	size_t length,
	long *highest)
{
	struct edit_line line;
	const char *field;
	size_t field_length;
	size_t at;
	long number;
	int more;
	int found;

	/* Each line's third field. */
	at = 0;
	for (;;) {
		/* The next line, or the end. */
		more = edit_next_line(text, length, at, &line);
		if (!more)
			return;
		at = line.next;

		/* A person's number. */
		found = edit_line_field(line.start, line.length, 2U, &field, &field_length);
		number = -1;
		if (found)
			number = edit_number(field, field_length);
		if (number < ADMIN_ID_FIRST || number > ADMIN_ID_LAST)
			continue;

		/* Higher than any so far. */
		if (number > *highest)
			*highest = number;
	}
}

/*
 * Reports whether a list of members (split by ',') names a user.
 */
static int
edit_has_member(
	const char *members,
	size_t length,
	const char *name)
{
	const char *comma;
	size_t at;
	size_t item;
	size_t name_length;
	int same;

	/* Each member. */
	name_length = strlen(name);
	at = 0;
	while (at < length) {
		/* One member, to the next ',' or the list's end. */
		comma = memchr(members + at, ',', length - at);
		item = length - at;
		if (comma != NULL)
			item = (size_t)(comma - (members + at));

		/* The name. */
		if (item == name_length) {
			same = memcmp(members + at, name, name_length);
			if (same == 0)
				return 1;
		}

		/* The next. */
		at += item + 1U;
	}

	/* Not named. */
	return 0;
}

/*
 * Writes a list of members again: every member but the name, then the name
 * at the end when it is to be one.
 */
static int
edit_members_rebuilt(
	const char *members,
	size_t length,
	const char *name,
	int member,
	char *output,
	size_t capacity,
	size_t *used)
{
	const char *comma;
	size_t name_length;
	size_t at;
	size_t item;
	int first;
	int same;
	int error;

	/* Each member but the name (empty items dropped). */
	name_length = strlen(name);
	first = 1;
	at = 0;
	while (at < length) {
		/* One member. */
		comma = memchr(members + at, ',', length - at);
		item = length - at;
		if (comma != NULL)
			item = (size_t)(comma - (members + at));

		/* The name itself, or nothing: left out. */
		same = 1;
		if (item == name_length)
			same = memcmp(members + at, name, name_length);
		if (item == 0U || same == 0) {
			at += item + 1U;
			continue;
		}

		/* Kept, after a ',' when not the first. */
		error = 0;
		if (!first)
			error = edit_put(output, capacity, used, ",", 1U);
		if (error == 0)
			error = edit_put(output, capacity, used, members + at, item);
		if (error != 0)
			return error;
		first = 0;
		at += item + 1U;
	}

	/* The name at the end when it is to be a member. */
	if (member) {
		error = 0;
		if (!first)
			error = edit_put(output, capacity, used, ",", 1U);
		if (error == 0)
			error = edit_put(output, capacity, used, name, name_length);
		if (error != 0)
			return error;
	}

	/* Succeeded: the list. */
	return 0;
}

/*
 * Reports whether a shell lets its user log in, as the desktop's list of
 * users judges it: none named is the default shell, and a nologin or a
 * false does not.
 */
static int
edit_shell_logs_in(
	const char *shell,
	size_t length)
{
	int refusing;

	/* No shell named: the default shell, which logs in. */
	if (length == 0U)
		return 1;

	/* A nologin. */
	refusing = edit_contains(shell, length, "nologin");
	if (refusing)
		return 0;

	/* A false, at the end of a path. */
	refusing = edit_contains(shell, length, "/false");
	if (refusing)
		return 0;

	/* Succeeded: a shell that logs in. */
	return 1;
}

/*
 * Reports whether a field (not ended by a NUL) holds a word anywhere.
 */
static int
edit_contains(
	const char *text,
	size_t length,
	const char *word)
{
	size_t word_length;
	size_t at;
	int same;

	/* A word longer than the field is not in it. */
	word_length = strlen(word);
	if (word_length > length)
		return 0;

	/* Each place the word could start. */
	for (at = 0; at + word_length <= length; at++) {
		same = memcmp(text + at, word, word_length);
		if (same == 0)
			return 1;
	}

	/* Not in it. */
	return 0;
}

/*
 * Reports whether a user's shadow line lets it log in on a day (days since
 * 1970), as passkey judges it: a line with a hash that is not empty and not
 * locked ('!' or '*' in front), and an account that has not expired (its
 * eighth field empty, 0, or a later day).  An expiry that is not a number
 * is taken as expired, so that the count of administrators errs low.
 */
static int
edit_login_usable(
	const char *shadow,
	size_t shadow_length,
	const char *name,
	long day)
{
	const char *hash;
	const char *expiry;
	size_t hash_length;
	size_t expiry_length;
	long expires;
	int error;

	/* The user's hash. */
	error = admin_field(shadow, shadow_length, name, 1U, &hash, &hash_length);
	if (error != 0)
		return 0;

	/* An empty hash, or a locked one. */
	if (hash_length == 0U)
		return 0;
	if (hash[0] == '!' || hash[0] == '*')
		return 0;

	/* A line without the expiry field never expires. */
	error = admin_field(shadow, shadow_length, name, 7U, &expiry, &expiry_length);
	if (error != 0)
		return 1;
	if (expiry_length == 0U)
		return 1;

	/* An expiry that is not a number. */
	expires = edit_number(expiry, expiry_length);
	if (expires < 0)
		return 0;

	/* An expiry reached. */
	if (expires > 0 && day >= expires)
		return 0;

	/* Succeeded: the user can log in. */
	return 1;
}

/*
 * Overwrites a buffer that held a hash.
 */
static void
edit_wipe(
	char *buffer,
	size_t size)
{
	volatile char *byte;
	size_t index;

	/* Byte by byte, so that the compiler keeps the writes. */
	byte = buffer;
	for (index = 0; index < size; index++)
		byte[index] = '\0';
}
