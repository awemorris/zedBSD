/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The rules of the admin actions: what a definition in
 * /etc/keiland/admin.d says and whether it can be trusted, which words a
 * request may carry, who is asked and how, and which permissions are
 * kept for later requests.
 *
 * A definition is one file per action, named after the action
 * ([a-z0-9][a-z0-9-]{0,31}), one "key value" line per item:
 *
 *   program /usr/libexec/install-admin   the program run as root
 *   argv probe                           an allowed set of words (1 to 16 lines)
 *   title Install zedBSD                 the dialog's title (title.ja ... too)
 *   message The installer will ...       what will happen (message.ja ... too)
 *   wheel consent|password|refuse        how root and wheel are asked (consent)
 *   others admin-password|refuse         how anyone else is asked (refuse)
 *   keep none|client|SECONDS             how long a permission is kept (none)
 *   streams input output error           the streams the program may get
 *   display none|wayland                 a desktop program: the compositor
 *                                        lends it a connection (none)
 *
 * Lines starting with '#' and empty lines are skipped.  A key not known,
 * a key given twice (but argv), a control character, a value too long
 * or a word not allowed makes the whole definition invalid, so that a
 * mistake never makes it looser.  A request's words must be one allowed
 * set exactly: there are no patterns.
 *
 * A definition and a program are opened from the root down, one
 * directory at a time without following a symbolic link, and every
 * directory and the file must belong to the owner and be writable by no
 * one else.
 */

#include "admin-rules.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* The longest key of a definition: "message." and a language tag. */
#define RULES_KEY_MAX		24U

/* The longest component of a path opened. */
#define RULES_COMPONENT_MAX	255U

/* The longest decimal number of a request (a 32-bit value). */
#define RULES_NUMBER_DIGITS	10U

/* The largest number a request carries. */
#define RULES_NUMBER_LARGEST	4294967295UL

/* How many words each of a definition's choices has. */
#define RULES_WHEEL_WORDS	3U
#define RULES_OTHERS_WORDS	2U
#define RULES_DISPLAY_WORDS	2U
#define RULES_STREAM_WORDS	3U

/*
 * The keys a definition has said once already: a second one of these
 * makes the definition invalid.  argv may come many times, and a title
 * or message twice in the same language is found among the texts.
 */
struct rules_seen {
	int program;
	int wheel;
	int others;
	int keep;
	int streams;
	int display;
};

/* The words of a definition's wheel, and the methods they name. */
static const char *const rules_wheel_words[RULES_WHEEL_WORDS] = { "consent", "password", "refuse" };
static const enum sessiond_admin_method rules_wheel_methods[RULES_WHEEL_WORDS] = {
	SESSIOND_ADMIN_CONSENT,
	SESSIOND_ADMIN_PASSWORD,
	SESSIOND_ADMIN_REFUSE
};

/* The words of a definition's others, and the methods they name. */
static const char *const rules_others_words[RULES_OTHERS_WORDS] = { "admin-password", "refuse" };
static const enum sessiond_admin_method rules_others_methods[RULES_OTHERS_WORDS] = {
	SESSIOND_ADMIN_ADMIN_PASSWORD,
	SESSIOND_ADMIN_REFUSE
};

/* The words of a definition's display: the index is whether a connection is lent. */
static const char *const rules_display_words[RULES_DISPLAY_WORDS] = { "none", "wayland" };

/* The words of a definition's streams, and their bits. */
static const char *const rules_stream_words[RULES_STREAM_WORDS] = { "input", "output", "error" };
static const unsigned rules_stream_bits[RULES_STREAM_WORDS] = {
	SESSIOND_ADMIN_INPUT,
	SESSIOND_ADMIN_OUTPUT,
	SESSIOND_ADMIN_ERROR
};

static int rules_line(struct sessiond_admin_definition *definition, struct rules_seen *seen, const char *line, size_t length);
static int rules_value(struct sessiond_admin_definition *definition, struct rules_seen *seen, const char *key, const char *value);
static int rules_text(struct sessiond_admin_text *texts, unsigned *count, const char *language, const char *value, size_t limit);
static int rules_argv(struct sessiond_admin_definition *definition, const char *value);
static int rules_streams(const char *value, unsigned *streams);
static int rules_keep(const char *value, enum sessiond_admin_keep_kind *kind, unsigned *seconds);
static int rules_program(const char *path);
static int rules_language(const char *language);
static int rules_locale(const char *locale);
static const char *rules_text_in(const struct sessiond_admin_text *texts, unsigned count, const char *language);
static int rules_word(const char *word, size_t length);
static int rules_word_byte(unsigned char byte);
static int rules_utf8(const char *text);
static int rules_number(const char *text, size_t length, unsigned long *number);
static size_t rules_token(const char *text);
static int rules_pick(const char *value, const char *const *words, unsigned count);
static int rules_trusted(const struct stat *status, uid_t owner);

/*
 * Says whether a name is an action's name: a lowercase letter or digit,
 * then up to 31 more of those or dashes.  A name with a dot never names
 * a hidden file, an editor's backup, or a parent directory.
 */
int
sessiond_admin_name_valid(
	const char *name)
{
	size_t length;
	size_t index;
	unsigned char byte;

	/* One to 32 bytes. */
	length = strlen(name);
	if (length == 0U || length > SESSIOND_ADMIN_NAME_MAX)
		return 0;

	/* Lowercase letters, digits and dashes, a dash never first. */
	for (index = 0; index < length; index++) {
		byte = (unsigned char)name[index];
		if (byte >= 'a' && byte <= 'z')
			continue;
		if (byte >= '0' && byte <= '9')
			continue;
		if (byte == '-' && index != 0U)
			continue;
		return 0;
	}

	/* Succeeded: the name may name an action. */
	return 1;
}

/*
 * Reads a definition (the file's text and its length) of the action
 * named.  Returns 0 with the definition, or EINVAL when any line breaks
 * the rules or something required is missing.
 */
int
sessiond_admin_parse(
	const char *name,
	const char *text,
	size_t length,
	struct sessiond_admin_definition *definition)
{
	struct rules_seen seen;
	size_t start;
	size_t end;
	int valid;
	int error;

	/* The defaults: wheel allows, anyone else is refused, nothing is kept, input and output. */
	memset(definition, 0, sizeof(*definition));
	memset(&seen, 0, sizeof(seen));
	definition->wheel = SESSIOND_ADMIN_CONSENT;
	definition->others = SESSIOND_ADMIN_REFUSE;
	definition->keep = SESSIOND_ADMIN_KEEP_NONE;
	definition->streams = SESSIOND_ADMIN_INPUT | SESSIOND_ADMIN_OUTPUT;

	/* The action's own name. */
	valid = sessiond_admin_name_valid(name);
	if (!valid)
		return EINVAL;
	(void)snprintf(definition->name, sizeof(definition->name), "%s", name);

	/* Each line, up to its line end or the end of the text. */
	start = 0;
	while (start < length) {
		end = start;
		while (end < length && text[end] != '\n')
			end++;

		/* One bad line spoils the whole definition. */
		error = rules_line(definition, &seen, text + start, end - start);
		if (error != 0)
			return EINVAL;
		start = end + 1U;
	}

	/* A program, at least one set of words, a title and a message are required. */
	if (!seen.program)
		return EINVAL;
	if (definition->row_count == 0U)
		return EINVAL;

	/* The texts without a language are the ones every dialog can fall back to. */
	if (rules_text_in(definition->titles, definition->title_count, "") == NULL)
		return EINVAL;
	if (rules_text_in(definition->messages, definition->message_count, "") == NULL)
		return EINVAL;

	/* Succeeded: the definition is whole. */
	return 0;
}

/*
 * Says whether a request's words are one of the definition's allowed
 * sets exactly: the same number of words, in the same order.  Returns 0
 * when they are, or ENOENT.
 */
int
sessiond_admin_match(
	const struct sessiond_admin_definition *definition,
	const struct sessiond_admin_words *words)
{
	const struct sessiond_admin_words *row;
	unsigned index;
	unsigned word;
	int differs;
	int same;

	/* Each allowed set until one is the same. */
	for (index = 0; index < definition->row_count; index++) {
		row = &definition->rows[index];
		if (row->count != words->count)
			continue;

		/* Word by word. */
		same = 1;
		for (word = 0; word < row->count; word++) {
			differs = strcmp(row->words[word], words->words[word]);
			if (differs != 0) {
				same = 0;
				break;
			}
		}

		/* Found. */
		if (same)
			return 0;
	}

	/* No allowed set is these words. */
	return ENOENT;
}

/*
 * Chooses a text for the desktop's locale ("ll_CC.UTF-8", or "" for
 * none): the text in the language and region (ll-CC), else in the
 * language (ll), else the one without a language.  Returns NULL when
 * there is none of them.
 */
const char *
sessiond_admin_text(
	const struct sessiond_admin_text *texts,
	unsigned count,
	const char *locale)
{
	char language[SESSIOND_ADMIN_LANGUAGE_MAX + 1U];
	const char *text;
	size_t letters;

	/* The language's letters. */
	letters = strcspn(locale, "_.");
	if (letters > 3U)
		letters = 0;
	memcpy(language, locale, letters);
	language[letters] = '\0';

	/* The language and its region, when the locale has one (rules_locale made it two capitals). */
	if (letters != 0U && locale[letters] == '_' && locale[letters + 1U] != '\0' && locale[letters + 2U] != '\0') {
		language[letters] = '-';
		language[letters + 1U] = locale[letters + 1U];
		language[letters + 2U] = locale[letters + 2U];
		language[letters + 3U] = '\0';
		text = rules_text_in(texts, count, language);
		if (text != NULL)
			return text;
		language[letters] = '\0';
	}

	/* The language alone. */
	if (letters != 0U) {
		text = rules_text_in(texts, count, language);
		if (text != NULL)
			return text;
	}

	/* The text without a language, if any. */
	text = rules_text_in(texts, count, "");
	return text;
}

/*
 * Decides how the session's user is asked for an action: root and the
 * members of wheel as the definition's wheel says, anyone else as its
 * others says.
 */
enum sessiond_admin_verdict
sessiond_admin_decide(
	const struct sessiond_admin_definition *definition,
	uid_t uid,
	int in_wheel)
{
	enum sessiond_admin_method method;

	/* Root and wheel are the administrators; anyone else is not. */
	method = definition->others;
	if (uid == 0 || in_wheel)
		method = definition->wheel;

	/* What the method asks of the dialog. */
	switch (method) {
	case SESSIOND_ADMIN_CONSENT:
		return SESSIOND_ADMIN_VERDICT_CONSENT;
	case SESSIOND_ADMIN_PASSWORD:
		return SESSIOND_ADMIN_VERDICT_PASSWORD_SELF;
	case SESSIOND_ADMIN_ADMIN_PASSWORD:
		return SESSIOND_ADMIN_VERDICT_PASSWORD_ADMIN;
	case SESSIOND_ADMIN_REFUSE:
		break;
	}

	/* Refused without a dialog. */
	return SESSIOND_ADMIN_VERDICT_NOT_ALLOWED;
}

/*
 * Reads a request's number at the front of a text: 1 to 4294967295 in
 * decimal, without a leading zero.  Returns 0 with the number and where
 * the text goes on after it, or EINVAL.
 */
int
sessiond_admin_parse_id(
	const char *text,
	unsigned long *id,
	const char **rest)
{
	size_t length;
	int error;

	/* The digits up to the next space or the end. */
	*id = 0;
	length = rules_token(text);
	error = rules_number(text, length, id);
	if (error != 0)
		return EINVAL;

	/* 0 is never a request's number. */
	if (*id == 0UL)
		return EINVAL;

	/* Succeeded: the number, and the rest after it. */
	*rest = text + length;
	return 0;
}

/*
 * Reads the arguments of an ADMIN ASK request (the line after
 * "ADMIN ASK "): "ID CLIENT LOCALE STREAMS ACTION [WORD...]", one
 * space between each.  LOCALE is the desktop's ("ll_CC.UTF-8", the
 * region and the codeset optional) or "-" for none, STREAMS a
 * hexadecimal set of the stream bits.  Returns 0 with the request, or
 * EINVAL; ask->id is the request's number whenever it could be read
 * (0 otherwise), so a refusal can name it.
 */
int
sessiond_admin_parse_ask(
	const char *arguments,
	struct sessiond_admin_ask *ask)
{
	const char *cursor;
	unsigned long number;
	unsigned long streams;
	size_t length;
	size_t joined;
	size_t index;
	unsigned char digit;
	int valid;
	int error;

	/* The request's number. */
	memset(ask, 0, sizeof(*ask));
	error = sessiond_admin_parse_id(arguments, &number, &cursor);
	if (error != 0)
		return EINVAL;
	ask->id = number;

	/* The client's number. */
	if (*cursor != ' ')
		return EINVAL;
	cursor++;
	length = rules_token(cursor);
	error = rules_number(cursor, length, &number);
	if (error != 0)
		return EINVAL;
	ask->client = number;
	cursor += length;

	/* The locale, or "-" for none. */
	if (*cursor != ' ')
		return EINVAL;
	cursor++;
	length = rules_token(cursor);
	if (length == 0U || length > SESSIOND_ADMIN_LOCALE_MAX)
		return EINVAL;
	memcpy(ask->locale, cursor, length);
	ask->locale[length] = '\0';
	if (length == 1U && cursor[0] == '-') {
		ask->locale[0] = '\0';
	} else {
		valid = rules_locale(ask->locale);
		if (!valid)
			return EINVAL;
	}
	cursor += length;

	/* The streams: one or two hexadecimal digits, no bit but the three. */
	if (*cursor != ' ')
		return EINVAL;
	cursor++;
	length = rules_token(cursor);
	if (length == 0U || length > 2U)
		return EINVAL;
	streams = 0;
	for (index = 0; index < length; index++) {
		digit = (unsigned char)cursor[index];
		streams *= 16UL;
		if (digit >= '0' && digit <= '9') {
			streams += (unsigned long)(digit - '0');
		} else if (digit >= 'a' && digit <= 'f') {
			streams += (unsigned long)(digit - 'a' + 10);
		} else {
			return EINVAL;
		}
	}
	if ((streams & ~(unsigned long)SESSIOND_ADMIN_STREAMS) != 0UL)
		return EINVAL;
	ask->streams = (unsigned)streams;
	cursor += length;

	/* The action's name. */
	if (*cursor != ' ')
		return EINVAL;
	cursor++;
	length = rules_token(cursor);
	if (length == 0U || length > SESSIOND_ADMIN_NAME_MAX)
		return EINVAL;
	memcpy(ask->action, cursor, length);
	ask->action[length] = '\0';
	valid = sessiond_admin_name_valid(ask->action);
	if (!valid)
		return EINVAL;
	cursor += length;

	/* The words, one space before each, within the counts and the bytes of a set. */
	joined = 0;
	while (*cursor != '\0') {
		if (*cursor != ' ')
			return EINVAL;
		cursor++;
		length = rules_token(cursor);
		valid = rules_word(cursor, length);
		if (!valid)
			return EINVAL;
		if (ask->words.count >= SESSIOND_ADMIN_WORDS_MAX)
			return EINVAL;

		/* The words joined by single spaces stay within the line's share. */
		if (ask->words.count != 0U)
			joined++;
		joined += length;
		if (joined > SESSIOND_ADMIN_WORDS_BYTES)
			return EINVAL;
		memcpy(ask->words.words[ask->words.count], cursor, length);
		ask->words.words[ask->words.count][length] = '\0';
		ask->words.count++;
		cursor += length;
	}

	/* Succeeded: the request is whole. */
	return 0;
}

/*
 * Opens a file below the place's root: every directory from the root
 * down is opened without following a symbolic link and must belong to
 * the place's owner and be writable by no one else, and so must the
 * file, which must be a regular file.  The path is absolute, without
 * "." or ".." or empty components.  Returns 0 with the open file (closed
 * on exec) and its status, or an errno value: ELOOP for a symbolic link,
 * EPERM for a directory or file that cannot be trusted, ENOENT, EINVAL.
 */
int
sessiond_admin_open(
	const struct sessiond_admin_place *place,
	const char *path,
	int *descriptor,
	struct stat *status)
{
	char component[RULES_COMPONENT_MAX + 1U];
	const char *cursor;
	const char *root;
	size_t length;
	int directory;
	int next;
	int trusted;
	int error;

	/* An absolute path. */
	*descriptor = -1;
	if (path[0] != '/')
		return EINVAL;

	/* The root, which must be trusted too. */
	root = place->root;
	if (root == NULL || root[0] == '\0')
		root = "/";
	directory = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (directory < 0)
		return errno;
	error = fstat(directory, status);
	if (error != 0) {
		error = errno;
		(void)close(directory);
		return error;
	}
	trusted = rules_trusted(status, place->owner);
	if (!trusted || !S_ISDIR(status->st_mode)) {
		(void)close(directory);
		return EPERM;
	}

	/* Each component in turn: a directory until the last, which is the file. */
	cursor = path + 1;
	for (;;) {
		length = strcspn(cursor, "/");
		if (length == 0U || length > RULES_COMPONENT_MAX) {
			(void)close(directory);
			return EINVAL;
		}
		memcpy(component, cursor, length);
		component[length] = '\0';

		/* "." and ".." would leave the walk. */
		if (component[0] == '.' && (component[1] == '\0' || (component[1] == '.' && component[2] == '\0'))) {
			(void)close(directory);
			return EINVAL;
		}

		/* The last component is the file. */
		if (cursor[length] == '\0')
			break;

		/* A directory on the way, not a link to one, and trusted. */
		next = openat(directory, component, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		error = errno;
		(void)close(directory);
		if (next < 0)
			return error;
		directory = next;
		error = fstat(directory, status);
		if (error != 0) {
			error = errno;
			(void)close(directory);
			return error;
		}
		trusted = rules_trusted(status, place->owner);
		if (!trusted) {
			(void)close(directory);
			return EPERM;
		}
		cursor += length + 1U;
	}

	/* The file itself, not a link and not a device or a pipe that would make the open wait. */
	next = openat(directory, component, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	error = errno;
	(void)close(directory);
	if (next < 0)
		return error;
	error = fstat(next, status);
	if (error != 0) {
		error = errno;
		(void)close(next);
		return error;
	}

	/* A regular file, the owner's, writable by no one else. */
	trusted = rules_trusted(status, place->owner);
	if (!trusted || !S_ISREG(status->st_mode)) {
		(void)close(next);
		return EPERM;
	}

	/* Succeeded: the file is open. */
	*descriptor = next;
	return 0;
}

/*
 * Reads the definition of the action named from the place's definition
 * directory.  Returns 0 with the definition and what the file was, or
 * ENOENT for a name that is no action's, an errno value of the open
 * (sessiond_admin_open), EFBIG for a file over 4096 bytes, EIO, or
 * EINVAL for a definition that breaks the rules.
 */
int
sessiond_admin_load(
	const struct sessiond_admin_place *place,
	const char *name,
	struct sessiond_admin_definition *definition,
	struct sessiond_admin_identity *identity)
{
	char text[SESSIOND_ADMIN_FILE_MAX + 1U];
	char path[sizeof(SESSIOND_ADMIN_DIRECTORY) + SESSIOND_ADMIN_NAME_MAX + 2U];
	struct stat status;
	size_t used;
	ssize_t count;
	int descriptor;
	int valid;
	int error;

	/* Only a valid name reaches the file system. */
	memset(identity, 0, sizeof(*identity));
	valid = sessiond_admin_name_valid(name);
	if (!valid)
		return ENOENT;
	(void)snprintf(path, sizeof(path), "%s/%s", SESSIOND_ADMIN_DIRECTORY, name);

	/* The file, trusted from the root down. */
	error = sessiond_admin_open(place, path, &descriptor, &status);
	if (error != 0)
		return error;

	/* Small, as a definition is. */
	if (status.st_size > (off_t)SESSIOND_ADMIN_FILE_MAX) {
		(void)close(descriptor);
		return EFBIG;
	}

	/* The whole text, a byte more than allowed being enough to see it is too long. */
	used = 0;
	for (;;) {
		count = read(descriptor, text + used, sizeof(text) - used);
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0) {
			(void)close(descriptor);
			return EIO;
		}
		if (count == 0)
			break;
		used += (size_t)count;
		if (used >= sizeof(text))
			break;
	}
	(void)close(descriptor);
	if (used > SESSIOND_ADMIN_FILE_MAX)
		return EFBIG;

	/* What it says. */
	error = sessiond_admin_parse(name, text, used, definition);
	if (error != 0)
		return EINVAL;

	/* Succeeded: the definition, and the file it came from. */
	sessiond_admin_identity_of(&status, identity);
	return 0;
}

/*
 * Opens a definition's program, trusted from the root down the same way
 * as a definition, and executable by its owner.  Returns 0 with the
 * program open (closed on exec; fexecve runs it), or an errno value.
 */
int
sessiond_admin_open_program(
	const struct sessiond_admin_place *place,
	const struct sessiond_admin_definition *definition,
	int *descriptor)
{
	struct stat status;
	int error;

	/* The program, trusted from the root down. */
	error = sessiond_admin_open(place, definition->program, descriptor, &status);
	if (error != 0)
		return error;

	/* Its owner may run it. */
	if ((status.st_mode & S_IXUSR) == 0) {
		(void)close(*descriptor);
		*descriptor = -1;
		return EACCES;
	}

	/* Succeeded: the program is open. */
	return 0;
}

/*
 * Notes what a definition file is: its device and inode, size and
 * modification time.
 */
void
sessiond_admin_identity_of(
	const struct stat *status,
	struct sessiond_admin_identity *identity)
{
	/* The fields a replacement or a change moves. */
	memset(identity, 0, sizeof(*identity));
	identity->device = status->st_dev;
	identity->inode = status->st_ino;
	identity->size = status->st_size;
	identity->modified = status->st_mtime;
}

/*
 * Forgets every kept permission.
 */
void
sessiond_admin_keep_clear(
	struct sessiond_admin_keep *keep)
{
	/* An empty table. */
	memset(keep, 0, sizeof(*keep));
}

/*
 * Looks for a kept permission of a client (of the session's generation)
 * for an action.  One that has run out, or whose definition file is no
 * longer what it was, is forgotten.  Returns 1 when the permission is
 * kept, 0 when it is not.
 */
int
sessiond_admin_keep_find(
	struct sessiond_admin_keep *keep,
	unsigned long long generation,
	unsigned long client,
	const char *action,
	const struct sessiond_admin_identity *identity,
	long long now_ms)
{
	struct sessiond_admin_kept *entry;
	unsigned index;
	int differs;

	/* Each entry of the same session, client and action. */
	for (index = 0; index < SESSIOND_ADMIN_KEEP_MAX; index++) {
		entry = &keep->entries[index];
		if (!entry->used)
			continue;
		if (entry->generation != generation || entry->client != client)
			continue;
		differs = strcmp(entry->action, action);
		if (differs != 0)
			continue;

		/* A permission kept for some seconds ends with them. */
		if (entry->kind == SESSIOND_ADMIN_KEEP_SECONDS && now_ms >= entry->expires_ms) {
			memset(entry, 0, sizeof(*entry));
			continue;
		}

		/* A definition replaced or changed is a new question. */
		if (entry->identity.device != identity->device ||
		    entry->identity.inode != identity->inode ||
		    entry->identity.size != identity->size ||
		    entry->identity.modified != identity->modified) {
			memset(entry, 0, sizeof(*entry));
			continue;
		}

		/* Kept. */
		return 1;
	}

	/* Not kept. */
	return 0;
}

/*
 * Keeps the permission just given to a client for an action, as its
 * definition says (nothing for keep none).  An entry for the same
 * session, client and action is replaced; a full table loses its oldest.
 */
void
sessiond_admin_keep_add(
	struct sessiond_admin_keep *keep,
	unsigned long long generation,
	unsigned long client,
	const struct sessiond_admin_definition *definition,
	const struct sessiond_admin_identity *identity,
	long long now_ms)
{
	struct sessiond_admin_kept *entry;
	struct sessiond_admin_kept *slot;
	unsigned index;
	int differs;

	/* A definition that keeps nothing. */
	if (definition->keep == SESSIOND_ADMIN_KEEP_NONE)
		return;

	/* The same key's entry, else an empty one, else the oldest. */
	slot = NULL;
	for (index = 0; index < SESSIOND_ADMIN_KEEP_MAX; index++) {
		entry = &keep->entries[index];
		if (!entry->used)
			continue;
		differs = strcmp(entry->action, definition->name);
		if (entry->generation == generation && entry->client == client && differs == 0) {
			slot = entry;
			break;
		}
	}
	for (index = 0; slot == NULL && index < SESSIOND_ADMIN_KEEP_MAX; index++) {
		if (!keep->entries[index].used)
			slot = &keep->entries[index];
	}
	if (slot == NULL) {
		slot = &keep->entries[0];
		for (index = 1; index < SESSIOND_ADMIN_KEEP_MAX; index++) {
			if (keep->entries[index].order < slot->order)
				slot = &keep->entries[index];
		}
	}

	/* The entry; the order only grows, so the smallest is always the oldest. */
	memset(slot, 0, sizeof(*slot));
	slot->used = 1;
	slot->generation = generation;
	slot->client = client;
	(void)snprintf(slot->action, sizeof(slot->action), "%s", definition->name);
	slot->kind = definition->keep;
	slot->expires_ms = now_ms + (long long)definition->keep_seconds * 1000LL;
	slot->identity = *identity;
	keep->order++;
	slot->order = keep->order;
}

/*
 * Forgets what a client was allowed: it has gone.
 */
void
sessiond_admin_keep_forget(
	struct sessiond_admin_keep *keep,
	unsigned long client)
{
	unsigned index;

	/* Every entry of the client. */
	for (index = 0; index < SESSIOND_ADMIN_KEEP_MAX; index++) {
		if (keep->entries[index].used && keep->entries[index].client == client)
			memset(&keep->entries[index], 0, sizeof(keep->entries[index]));
	}
}

/*
 * Forgets the permissions whose seconds have run out.
 */
void
sessiond_admin_keep_expire(
	struct sessiond_admin_keep *keep,
	long long now_ms)
{
	struct sessiond_admin_kept *entry;
	unsigned index;

	/* Every entry kept for seconds that are over. */
	for (index = 0; index < SESSIOND_ADMIN_KEEP_MAX; index++) {
		entry = &keep->entries[index];
		if (entry->used && entry->kind == SESSIOND_ADMIN_KEEP_SECONDS && now_ms >= entry->expires_ms)
			memset(entry, 0, sizeof(*entry));
	}
}

/* Reads one line of a definition (without its line end); returns 0 or EINVAL. */
static int
rules_line(
	struct sessiond_admin_definition *definition,
	struct rules_seen *seen,
	const char *line,
	size_t length)
{
	char key[RULES_KEY_MAX + 1U];
	char value[SESSIOND_ADMIN_MESSAGE_MAX + 1U];
	unsigned char byte;
	size_t index;
	size_t start;
	size_t end;
	size_t key_end;
	int error;

	/* No control character but the tab: a CR, a NUL or an escape spoils the definition. */
	for (index = 0; index < length; index++) {
		byte = (unsigned char)line[index];
		if (byte == '\t')
			continue;
		if (byte < 0x20U || byte == 0x7fU)
			return EINVAL;
	}

	/* The blanks around the line go. */
	start = 0;
	while (start < length && (line[start] == ' ' || line[start] == '\t'))
		start++;
	end = length;
	while (end > start && (line[end - 1U] == ' ' || line[end - 1U] == '\t'))
		end--;

	/* An empty line and a comment say nothing. */
	if (start == end || line[start] == '#')
		return 0;

	/* The key, up to the first blank. */
	key_end = start;
	while (key_end < end && line[key_end] != ' ' && line[key_end] != '\t')
		key_end++;
	if (key_end - start > RULES_KEY_MAX)
		return EINVAL;
	memcpy(key, line + start, key_end - start);
	key[key_end - start] = '\0';

	/* The value, after the blanks that follow the key. */
	while (key_end < end && (line[key_end] == ' ' || line[key_end] == '\t'))
		key_end++;
	if (end - key_end > SESSIOND_ADMIN_MESSAGE_MAX)
		return EINVAL;
	memcpy(value, line + key_end, end - key_end);
	value[end - key_end] = '\0';

	/* What the key says. */
	error = rules_value(definition, seen, key, value);
	if (error != 0)
		return EINVAL;

	/* Succeeded: the line is understood. */
	return 0;
}

/* Takes one key and its value into the definition; returns 0 or EINVAL. */
static int
rules_value(
	struct sessiond_admin_definition *definition,
	struct rules_seen *seen,
	const char *key,
	const char *value)
{
	const char *language;
	size_t length;
	int differs;
	int choice;
	int valid;
	int error;

	/* argv: an allowed set of words, as many lines as there are sets. */
	differs = strcmp(key, "argv");
	if (differs == 0) {
		error = rules_argv(definition, value);
		if (error != 0)
			return EINVAL;
		return 0;
	}

	/* program: the absolute path of the program, once. */
	differs = strcmp(key, "program");
	if (differs == 0) {
		if (seen->program)
			return EINVAL;
		seen->program = 1;
		valid = rules_program(value);
		if (!valid)
			return EINVAL;

		/* rules_program kept it shorter than the room. */
		length = strlen(value);
		memcpy(definition->program, value, length + 1U);
		return 0;
	}

	/* title and title.LANGUAGE. */
	language = NULL;
	differs = strcmp(key, "title");
	if (differs == 0)
		language = "";
	differs = strncmp(key, "title.", 6U);
	if (differs == 0)
		language = key + 6;
	if (language != NULL) {
		error = rules_text(definition->titles, &definition->title_count, language, value, SESSIOND_ADMIN_TITLE_MAX);
		if (error != 0)
			return EINVAL;
		return 0;
	}

	/* message and message.LANGUAGE. */
	differs = strcmp(key, "message");
	if (differs == 0)
		language = "";
	differs = strncmp(key, "message.", 8U);
	if (differs == 0)
		language = key + 8;
	if (language != NULL) {
		error = rules_text(definition->messages, &definition->message_count, language, value, SESSIOND_ADMIN_MESSAGE_MAX);
		if (error != 0)
			return EINVAL;
		return 0;
	}

	/* wheel: how root and wheel are asked, once. */
	differs = strcmp(key, "wheel");
	if (differs == 0) {
		if (seen->wheel)
			return EINVAL;
		seen->wheel = 1;
		choice = rules_pick(value, rules_wheel_words, RULES_WHEEL_WORDS);
		if (choice < 0)
			return EINVAL;
		definition->wheel = rules_wheel_methods[choice];
		return 0;
	}

	/* others: how anyone else is asked, once. */
	differs = strcmp(key, "others");
	if (differs == 0) {
		if (seen->others)
			return EINVAL;
		seen->others = 1;
		choice = rules_pick(value, rules_others_words, RULES_OTHERS_WORDS);
		if (choice < 0)
			return EINVAL;
		definition->others = rules_others_methods[choice];
		return 0;
	}

	/* keep: how long a permission is kept, once. */
	differs = strcmp(key, "keep");
	if (differs == 0) {
		if (seen->keep)
			return EINVAL;
		seen->keep = 1;
		error = rules_keep(value, &definition->keep, &definition->keep_seconds);
		if (error != 0)
			return EINVAL;
		return 0;
	}

	/* display: whether the program is the desktop's and is lent a connection, once. */
	differs = strcmp(key, "display");
	if (differs == 0) {
		if (seen->display)
			return EINVAL;
		seen->display = 1;
		choice = rules_pick(value, rules_display_words, RULES_DISPLAY_WORDS);
		if (choice < 0)
			return EINVAL;
		definition->display = choice;
		return 0;
	}

	/* streams: the streams the program may be given, once. */
	differs = strcmp(key, "streams");
	if (differs == 0) {
		if (seen->streams)
			return EINVAL;
		seen->streams = 1;
		error = rules_streams(value, &definition->streams);
		if (error != 0)
			return EINVAL;
		return 0;
	}

	/* A key not known: a mistake must not make the definition looser. */
	return EINVAL;
}

/* Takes a title or a message in a language ("" for none); returns 0 or EINVAL. */
static int
rules_text(
	struct sessiond_admin_text *texts,
	unsigned *count,
	const char *language,
	const char *value,
	size_t limit)
{
	size_t length;
	unsigned index;
	int differs;
	int valid;

	/* A language tag, when there is one. */
	if (language[0] != '\0') {
		valid = rules_language(language);
		if (!valid)
			return EINVAL;
	}

	/* A text, within its limit, of whole UTF-8 characters without control characters. */
	length = strlen(value);
	if (length == 0U || length > limit)
		return EINVAL;
	valid = rules_utf8(value);
	if (!valid)
		return EINVAL;

	/* Once per language. */
	for (index = 0; index < *count; index++) {
		differs = strcmp(texts[index].language, language);
		if (differs == 0)
			return EINVAL;
	}

	/* Room for it. */
	if (*count >= SESSIOND_ADMIN_LANGUAGES_MAX + 1U)
		return EINVAL;
	(void)snprintf(texts[*count].language, sizeof(texts[*count].language), "%s", language);
	(void)snprintf(texts[*count].text, sizeof(texts[*count].text), "%s", value);
	(*count)++;

	/* Succeeded: the text is taken. */
	return 0;
}

/* Takes an allowed set of words (none for an empty value); returns 0 or EINVAL. */
static int
rules_argv(
	struct sessiond_admin_definition *definition,
	const char *value)
{
	struct sessiond_admin_words *row;
	const char *cursor;
	size_t length;
	size_t joined;
	int valid;

	/* Room for another set. */
	if (definition->row_count >= SESSIOND_ADMIN_ROWS_MAX)
		return EINVAL;
	row = &definition->rows[definition->row_count];
	memset(row, 0, sizeof(*row));

	/* Each word between blanks, within the counts and the bytes of a set. */
	joined = 0;
	cursor = value;
	for (;;) {
		while (*cursor == ' ' || *cursor == '\t')
			cursor++;
		if (*cursor == '\0')
			break;
		length = strcspn(cursor, " \t");
		valid = rules_word(cursor, length);
		if (!valid)
			return EINVAL;
		if (row->count >= SESSIOND_ADMIN_WORDS_MAX)
			return EINVAL;

		/* The words joined by single spaces, as a request carries them. */
		if (row->count != 0U)
			joined++;
		joined += length;
		if (joined > SESSIOND_ADMIN_WORDS_BYTES)
			return EINVAL;
		memcpy(row->words[row->count], cursor, length);
		row->words[row->count][length] = '\0';
		row->count++;
		cursor += length;
	}

	/* Succeeded: one more allowed set. */
	definition->row_count++;
	return 0;
}

/* Reads the streams a definition allows: input, output and error, each at most once; returns 0 or EINVAL. */
static int
rules_streams(
	const char *value,
	unsigned *streams)
{
	char word[8];
	const char *cursor;
	size_t length;
	unsigned bit;
	int choice;

	/* Each word between blanks. */
	*streams = 0;
	cursor = value;
	for (;;) {
		while (*cursor == ' ' || *cursor == '\t')
			cursor++;
		if (*cursor == '\0')
			break;
		length = strcspn(cursor, " \t");
		if (length >= sizeof(word))
			return EINVAL;
		memcpy(word, cursor, length);
		word[length] = '\0';

		/* One of the three. */
		choice = rules_pick(word, rules_stream_words, RULES_STREAM_WORDS);
		if (choice < 0)
			return EINVAL;
		bit = rules_stream_bits[choice];

		/* Not twice. */
		if ((*streams & bit) != 0U)
			return EINVAL;
		*streams |= bit;
		cursor += length;
	}

	/* At least one. */
	if (*streams == 0U)
		return EINVAL;

	/* Succeeded: the streams. */
	return 0;
}

/* Reads how long a permission is kept: none, client, or 1 to 300 seconds; returns 0 or EINVAL. */
static int
rules_keep(
	const char *value,
	enum sessiond_admin_keep_kind *kind,
	unsigned *seconds)
{
	unsigned long number;
	int differs;
	int error;

	/* Never kept. */
	*seconds = 0;
	differs = strcmp(value, "none");
	if (differs == 0) {
		*kind = SESSIOND_ADMIN_KEEP_NONE;
		return 0;
	}

	/* Kept while the client is connected. */
	differs = strcmp(value, "client");
	if (differs == 0) {
		*kind = SESSIOND_ADMIN_KEEP_CLIENT;
		return 0;
	}

	/* Kept for some seconds. */
	error = rules_number(value, strlen(value), &number);
	if (error != 0)
		return EINVAL;
	if (number == 0UL || number > SESSIOND_ADMIN_KEEP_SECONDS_MAX)
		return EINVAL;
	*kind = SESSIOND_ADMIN_KEEP_SECONDS;
	*seconds = (unsigned)number;

	/* Succeeded: kept for the seconds. */
	return 0;
}

/*
 * Says whether a program path is absolute and plain: components of
 * letters, digits, '.', '_', '+' and '-', none empty, "." or "..".
 */
static int
rules_program(
	const char *path)
{
	const char *cursor;
	unsigned char byte;
	size_t length;
	size_t index;

	/* Absolute, with room for its end. */
	length = strlen(path);
	if (length < 2U || length >= SESSIOND_ADMIN_PATH_MAX)
		return 0;
	if (path[0] != '/')
		return 0;

	/* Each component. */
	cursor = path + 1;
	for (;;) {
		length = strcspn(cursor, "/");
		if (length == 0U)
			return 0;
		if (length == 1U && cursor[0] == '.')
			return 0;
		if (length == 2U && cursor[0] == '.' && cursor[1] == '.')
			return 0;

		/* Plain bytes only. */
		for (index = 0; index < length; index++) {
			byte = (unsigned char)cursor[index];
			if (byte >= 'a' && byte <= 'z')
				continue;
			if (byte >= 'A' && byte <= 'Z')
				continue;
			if (byte >= '0' && byte <= '9')
				continue;
			if (byte == '.' || byte == '_' || byte == '+' || byte == '-')
				continue;
			return 0;
		}

		/* The last component ends the path. */
		if (cursor[length] == '\0')
			break;
		cursor += length + 1U;
	}

	/* Succeeded: a plain absolute path. */
	return 1;
}

/* Says whether a language tag is two or three lowercase letters, then maybe a dash and 2 to 8 letters or digits. */
static int
rules_language(
	const char *language)
{
	unsigned char byte;
	size_t letters;
	size_t length;
	size_t index;

	/* The language's letters. */
	letters = 0;
	while (language[letters] >= 'a' && language[letters] <= 'z')
		letters++;
	if (letters < 2U || letters > 3U)
		return 0;

	/* Nothing more. */
	if (language[letters] == '\0')
		return 1;

	/* Or a dash and a region or a script. */
	if (language[letters] != '-')
		return 0;
	length = strlen(language + letters + 1U);
	if (length < 2U || length > 8U)
		return 0;
	for (index = letters + 1U; language[index] != '\0'; index++) {
		byte = (unsigned char)language[index];
		if (byte >= 'a' && byte <= 'z')
			continue;
		if (byte >= 'A' && byte <= 'Z')
			continue;
		if (byte >= '0' && byte <= '9')
			continue;
		return 0;
	}

	/* Succeeded: a language tag. */
	return 1;
}

/* Says whether a locale is two or three lowercase letters, maybe "_" and two capitals, maybe ".UTF-8". */
static int
rules_locale(
	const char *locale)
{
	size_t letters;
	int differs;

	/* The language's letters. */
	letters = 0;
	while (locale[letters] >= 'a' && locale[letters] <= 'z')
		letters++;
	if (letters < 2U || letters > 3U)
		return 0;
	locale += letters;

	/* A region of two capitals. */
	if (locale[0] == '_') {
		if (locale[1] < 'A' || locale[1] > 'Z')
			return 0;
		if (locale[2] < 'A' || locale[2] > 'Z')
			return 0;
		locale += 3;
	}

	/* Nothing more, or the codeset every locale of the system has. */
	if (locale[0] == '\0')
		return 1;
	differs = strcmp(locale, ".UTF-8");
	if (differs != 0)
		return 0;

	/* Succeeded: a locale. */
	return 1;
}

/* Reports the text in exactly a language ("" for the one without), or NULL. */
static const char *
rules_text_in(
	const struct sessiond_admin_text *texts,
	unsigned count,
	const char *language)
{
	unsigned index;
	int differs;

	/* The text whose language is the same. */
	for (index = 0; index < count; index++) {
		differs = strcmp(texts[index].language, language);
		if (differs == 0)
			return texts[index].text;
	}

	/* None in that language. */
	return NULL;
}

/* Says whether a word (of a length) is 1 to 64 of the bytes a word may have. */
static int
rules_word(
	const char *word,
	size_t length)
{
	size_t index;
	int allowed;

	/* Within its length. */
	if (length == 0U || length > SESSIOND_ADMIN_WORD_MAX)
		return 0;

	/* Each byte. */
	for (index = 0; index < length; index++) {
		allowed = rules_word_byte((unsigned char)word[index]);
		if (!allowed)
			return 0;
	}

	/* Succeeded: a word. */
	return 1;
}

/* Says whether a byte may be in a word: letters, digits and . _ / : = @ + , - */
static int
rules_word_byte(
	unsigned char byte)
{
	/* Letters and digits. */
	if (byte >= 'a' && byte <= 'z')
		return 1;
	if (byte >= 'A' && byte <= 'Z')
		return 1;
	if (byte >= '0' && byte <= '9')
		return 1;

	/* The punctuation of names, paths and settings, never a quote, a blank or a shell's special byte. */
	if (byte == '.' || byte == '_' || byte == '/' || byte == ':' || byte == '=')
		return 1;
	if (byte == '@' || byte == '+' || byte == ',' || byte == '-')
		return 1;

	/* Any other byte. */
	return 0;
}

/*
 * Says whether a text is whole UTF-8 (no overlong form, surrogate, or
 * code point past U+10FFFF) without a control character (C0, DEL or C1).
 */
static int
rules_utf8(
	const char *text)
{
	const unsigned char *byte;
	unsigned long point;
	unsigned long smallest;
	unsigned follow;
	unsigned index;

	/* Character by character. */
	byte = (const unsigned char *)text;
	while (*byte != 0U) {
		/* One byte: no control character. */
		if (*byte < 0x80U) {
			if (*byte < 0x20U || *byte == 0x7fU)
				return 0;
			byte++;
			continue;
		}

		/* The lead byte says how many follow, and the smallest value that needs them. */
		if ((*byte & 0xe0U) == 0xc0U) {
			point = *byte & 0x1fU;
			follow = 1;
			smallest = 0x80UL;
		} else if ((*byte & 0xf0U) == 0xe0U) {
			point = *byte & 0x0fU;
			follow = 2;
			smallest = 0x800UL;
		} else if ((*byte & 0xf8U) == 0xf0U) {
			point = *byte & 0x07U;
			follow = 3;
			smallest = 0x10000UL;
		} else {
			return 0;
		}

		/* The following bytes. */
		for (index = 1; index <= follow; index++) {
			if ((byte[index] & 0xc0U) != 0x80U)
				return 0;
			point = (point << 6) | (byte[index] & 0x3fUL);
		}

		/* Not overlong, not a surrogate, within Unicode, not a C1 control. */
		if (point < smallest)
			return 0;
		if (point >= 0xd800UL && point <= 0xdfffUL)
			return 0;
		if (point > 0x10ffffUL)
			return 0;
		if (point >= 0x80UL && point <= 0x9fUL)
			return 0;
		byte += follow + 1U;
	}

	/* Succeeded: whole UTF-8 without control characters. */
	return 1;
}

/*
 * Reads a decimal number of a length: 1 to 10 digits, no leading zero
 * but in "0", at most 4294967295.  Returns 0 with the number, or EINVAL.
 */
static int
rules_number(
	const char *text,
	size_t length,
	unsigned long *number)
{
	unsigned long value;
	size_t index;

	/* One to ten digits, without a leading zero. */
	*number = 0;
	if (length == 0U || length > RULES_NUMBER_DIGITS)
		return EINVAL;
	if (length > 1U && text[0] == '0')
		return EINVAL;

	/* The value, which stays within 32 bits. */
	value = 0;
	for (index = 0; index < length; index++) {
		if (text[index] < '0' || text[index] > '9')
			return EINVAL;
		value = value * 10UL + (unsigned long)(text[index] - '0');
	}
	if (value > RULES_NUMBER_LARGEST)
		return EINVAL;

	/* Succeeded: the number. */
	*number = value;
	return 0;
}

/* Reports the length of a request's token: the bytes up to the next space or the end. */
static size_t
rules_token(
	const char *text)
{
	size_t length;

	/* Up to a space or the end. */
	length = strcspn(text, " ");

	/* The token's length (0 for none). */
	return length;
}

/* Says whether a directory or a file belongs to the owner and is writable by no one else. */
static int
rules_trusted(
	const struct stat *status,
	uid_t owner)
{
	/* The owner's. */
	if (status->st_uid != owner)
		return 0;

	/* Not writable by the group or anyone else. */
	if ((status->st_mode & (S_IWGRP | S_IWOTH)) != 0)
		return 0;

	/* Succeeded: trusted. */
	return 1;
}

/* Reports which of the words a value is (its index), or -1 for none of them. */
static int
rules_pick(
	const char *value,
	const char *const *words,
	unsigned count)
{
	unsigned index;
	int differs;

	/* Each word in turn. */
	for (index = 0; index < count; index++) {
		differs = strcmp(value, words[index]);
		if (differs == 0)
			return (int)index;
	}

	/* None of them. */
	return -1;
}
