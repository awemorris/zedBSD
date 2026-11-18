/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The translations of the user interface's text (WS158, kl_tr_*).
 *
 * A catalog is a UTF-8 text file, LANGUAGE/DOMAIN.tr under the locale
 * directory.  A line is a comment ('#'), empty, or an entry whose fields a
 * TAB separates:
 *
 *     msg     ENGLISH     TEXT
 *     ctx     CONTEXT     ENGLISH     TEXT
 *     plural  SINGULAR    PLURAL      FORM [FORM ...]
 *
 * A field writes a TAB as \t, a line's end as \n and a backslash as \\.
 * A line that is none of these, or an entry with an empty field, is left
 * out.  The forms of a plural are the language's, in the order its rule
 * numbers them (one form for Japanese; singular and plural for English).
 *
 * The file is read whole and its fields are decoded in place; the entries
 * point into that copy, and an open-addressing table of their hashes finds
 * them.  A program has its own domain's catalog and the shared one
 * (KL_TR_SHARED_DOMAIN), looked in in that order.  Everything belongs to
 * the one thread that uses the library's windows, except that kl_tr_read
 * reads catalogs on any thread into a record of their own, which that
 * thread then uses (kl_tr_use) or frees (kl_tr_discard).
 */

#include <keiland/keiland.h>

#include "userland/desktop/paths.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Where the installed catalogs are: LANGUAGE/DOMAIN.tr under it. */
#define TR_DIRECTORY		KEILAND_DATADIR "/keiland/locale"

/* The largest catalog read, and the longest path built to one. */
#define TR_FILE_MAX		(4U * 1024U * 1024U)
#define TR_PATH_MAX		512U

/* The most fields of an entry: the kind, and a plural's two keys and its forms. */
#define TR_FIELDS_MAX		12U

/* The places of kl_tr_format: {1} to {9}. */
#define TR_PLACES_MAX		9U

/* The catalogs a program has: its own domain's and the shared one. */
#define TR_CATALOGS		2U

/* FNV-1a's offset basis and prime (32 bits), the hash of the table. */
#define TR_HASH_BASIS		2166136261U
#define TR_HASH_PRIME		16777619U

/* The kinds of entries, which keep a context's or a plural's key apart from a plain one. */
enum tr_kind {
	TR_KIND_MESSAGE,
	TR_KIND_CONTEXT,
	TR_KIND_PLURAL
};

/*
 * One translation of a catalog.  context is set for a context's entry and
 * plural for a plural's; forms holds form_count texts one after another,
 * each ending with its NUL.  Every pointer is into the catalog's text.
 */
struct tr_entry {
	enum tr_kind kind;
	const char *context;
	const char *english;
	const char *plural;
	const char *forms;
	unsigned form_count;
	uint32_t hash;
};

/*
 * One catalog read: the file's decoded text, its entries, and the table
 * that finds them (slot_count is a power of two, at least twice the
 * entries; a slot holds an entry's index plus one, 0 for an empty slot).
 */
struct tr_catalog {
	char *text;
	struct tr_entry *entries;
	size_t count;
	uint32_t *slots;
	size_t slot_count;
};

/*
 * The program's translations: the language and domain read, and their
 * catalogs (empty ones in English or when a file is missing).
 */
struct tr_state {
	char language[KL_TR_NAME_MAX + 1U];
	char domain[KL_TR_NAME_MAX + 1U];
	struct tr_catalog catalogs[TR_CATALOGS];
};

/* A language's catalogs kl_tr_read read and nobody uses yet. */
struct kl_tr_catalogs {
	struct tr_state state;
};

/*
 * The languages ui.language numbers, in its order: 0 is English, the
 * texts of the source.
 */
static const char *const tr_languages[] = { "en", "ja" };

/*
 * The program's translations.  kl_tr_open fills it and kl_tr_close empties
 * it; its zero value (an empty language) reads as English.  Only the
 * thread that uses the library's windows touches it.
 */
static struct tr_state tr_state;

static int tr_state_read(struct tr_state *state, const char *directory, const char *domain, const char *language);
static void tr_state_use(const struct tr_state *state);
static int tr_name_valid(const char *name);
static int tr_catalog_read(struct tr_catalog *catalog, const char *directory, const char *language, const char *domain);
static int tr_catalog_parse(struct tr_catalog *catalog);
static int tr_catalog_index(struct tr_catalog *catalog);
static void tr_catalog_free(struct tr_catalog *catalog);
static int tr_entry_add(struct tr_catalog *catalog, char **fields, unsigned count, size_t *capacity);
static unsigned tr_split(char *line, char **fields, unsigned most);
static void tr_unescape(char *field);
static uint32_t tr_hash(enum tr_kind kind, const char *context, const char *english, const char *plural);
static uint32_t tr_hash_text(uint32_t hash, const char *text);
static const struct tr_entry *tr_find(enum tr_kind kind, const char *context, const char *english, const char *plural);
static const struct tr_entry *tr_catalog_find(const struct tr_catalog *catalog, enum tr_kind kind, const char *context, const char *english, const char *plural, uint32_t hash);
static int tr_key_same(const struct tr_entry *entry, const char *context, const char *english, const char *plural);
static int tr_same(const char *left, const char *right);
static unsigned tr_plural_form(const char *language, unsigned long count);
static const char *tr_form(const struct tr_entry *entry, unsigned form);
static int tr_put(char *out, size_t size, size_t *used, const char *text, size_t length);
static unsigned tr_place(const char *at);

/*
 * Reads a program's catalogs in a language from the installed directory.
 */
int
kl_tr_open(
	const char *domain,
	const char *language)
{
	int error;

	/* The installed catalogs. */
	error = kl_tr_open_directory(TR_DIRECTORY, domain, language);
	if (error != 0)
		return error;

	/* Succeeded: the texts are the language's. */
	return 0;
}

/*
 * Reads a program's catalogs in a language from a directory.
 */
int
kl_tr_open_directory(
	const char *directory,
	const char *domain,
	const char *language)
{
	struct tr_state fresh;
	int error;

	/* The catalogs read; a failure leaves every text in English. */
	error = tr_state_read(&fresh, directory, domain, language);
	if (error != 0) {
		kl_tr_close();
		return error;
	}

	/* Succeeded: the old catalogs go (the texts they gave are no longer valid). */
	tr_state_use(&fresh);
	return 0;
}

/*
 * Reads a program's catalogs in a language from the installed directory
 * into a record of its own, on any thread: the texts in use are not
 * touched.
 */
int
kl_tr_read(
	const char *domain,
	const char *language,
	struct kl_tr_catalogs **out)
{
	struct kl_tr_catalogs *catalogs;
	int error;

	/* Nothing until it is read. */
	if (out == NULL)
		return EINVAL;
	*out = NULL;

	/* The record. */
	catalogs = calloc(1U, sizeof(*catalogs));
	if (catalogs == NULL)
		return ENOMEM;

	/* The catalogs, into the record. */
	error = tr_state_read(&catalogs->state, TR_DIRECTORY, domain, language);
	if (error != 0) {
		free(catalogs);
		return error;
	}

	/* Succeeded: the caller's until kl_tr_use or kl_tr_discard. */
	*out = catalogs;
	return 0;
}

/*
 * Makes the catalogs kl_tr_read read the ones in use, on the thread that
 * uses the texts, and frees their record.
 */
void
kl_tr_use(
	struct kl_tr_catalogs *catalogs)
{
	/* Nothing read. */
	if (catalogs == NULL)
		return;

	/* The old catalogs go, then the record that carried the new ones. */
	tr_state_use(&catalogs->state);
	free(catalogs);
}

/*
 * Frees catalogs kl_tr_read read that are not to be used.
 */
void
kl_tr_discard(
	struct kl_tr_catalogs *catalogs)
{
	unsigned index;

	/* Nothing read. */
	if (catalogs == NULL)
		return;

	/* Each catalog's memory, then the record. */
	for (index = 0; index < TR_CATALOGS; index++)
		tr_catalog_free(&catalogs->state.catalogs[index]);
	free(catalogs);
}

/*
 * Forgets the catalogs: every text is English again.
 */
void
kl_tr_close(
	void)
{
	unsigned index;

	/* Each catalog's memory, then the names. */
	for (index = 0; index < TR_CATALOGS; index++)
		tr_catalog_free(&tr_state.catalogs[index]);
	memset(&tr_state, 0, sizeof(tr_state));
}

/*
 * Reports the language read ("en" before any).
 */
const char *
kl_tr_language(
	void)
{
	/* Nothing read is English. */
	if (tr_state.language[0] == '\0')
		return "en";

	/* The language of the catalogs. */
	return tr_state.language;
}

/*
 * Reports the language a value of ui.language names, or NULL.
 */
const char *
kl_tr_language_code(
	int setting)
{
	size_t count;

	/* Only the values the setting has. */
	count = sizeof(tr_languages) / sizeof(tr_languages[0]);
	if (setting < 0)
		return NULL;
	if ((size_t)setting >= count)
		return NULL;

	/* The language's code. */
	return tr_languages[setting];
}

/*
 * Gives the text of the language for an English text.
 */
const char *
kl_tr(
	const char *english)
{
	const struct tr_entry *entry;

	/* No text has no translation. */
	if (english == NULL)
		return "";

	/* The catalogs' text, or the English itself. */
	entry = tr_find(TR_KIND_MESSAGE, NULL, english, NULL);
	if (entry == NULL)
		return english;

	/* Succeeded: the language's text. */
	return entry->forms;
}

/*
 * Gives the text for an English text in a context.
 */
const char *
kl_trc(
	const char *context,
	const char *english)
{
	const struct tr_entry *entry;

	/* No text has no translation; no context is a plain text. */
	if (english == NULL)
		return "";
	if (context == NULL)
		return kl_tr(english);

	/* The catalogs' text, or the English itself. */
	entry = tr_find(TR_KIND_CONTEXT, context, english, NULL);
	if (entry == NULL)
		return english;

	/* Succeeded: the language's text. */
	return entry->forms;
}

/*
 * Gives the text for a number of things.
 */
const char *
kl_trn(
	const char *singular,
	const char *plural,
	unsigned long count)
{
	const struct tr_entry *entry;
	const char *english;
	unsigned form;

	/* No text has no translation. */
	if (singular == NULL || plural == NULL)
		return "";

	/* English's own rule: one is singular, every other number plural. */
	english = plural;
	if (count == 1UL)
		english = singular;

	/* The catalogs' forms, or the English. */
	entry = tr_find(TR_KIND_PLURAL, NULL, singular, plural);
	if (entry == NULL)
		return english;

	/* The language's form for the number. */
	form = tr_plural_form(kl_tr_language(), count);

	/* Succeeded: the form of the language. */
	return tr_form(entry, form);
}

/*
 * Writes a text of the language with its places filled.
 */
int
kl_tr_format(
	char *out,
	size_t size,
	const char *pattern,
	...)
{
	const char *strings[TR_PLACES_MAX];
	const char *string;
	const char *at;
	va_list arguments;
	unsigned count;
	unsigned place;
	size_t used;
	int error;
	int refused;
	int put;

	/* A buffer is needed for the NUL at least. */
	if (out == NULL || size == 0U)
		return EINVAL;
	out[0] = '\0';
	if (pattern == NULL)
		return EINVAL;

	/* The strings after the pattern, as far as the NULL (at most nine). */
	count = 0;
	va_start(arguments, pattern);
	for (;;) {
		string = va_arg(arguments, const char *);
		if (string == NULL || count == TR_PLACES_MAX)
			break;
		strings[count] = string;
		count++;
	}

	/* The arguments are read. */
	va_end(arguments);

	/* The pattern, its places replaced by their strings. */
	used = 0;
	error = 0;
	refused = 0;
	at = pattern;
	while (*at != '\0') {
		/* What comes next: a doubled brace, a place, or a byte of the text's own. */
		place = tr_place(at);
		if (at[0] == '{' && at[1] == '{') {
			/* A doubled brace is one brace. */
			put = tr_put(out, size, &used, "{", 1U);
			at += 2;
		} else if (place == 0U) {
			/* Any other byte is the text's own. */
			put = tr_put(out, size, &used, at, 1U);
			at++;
		} else if (place > count) {
			/* A place without its string. */
			put = 0;
			refused = 1;
			at += 3;
		} else {
			/* A place: its string. */
			put = tr_put(out, size, &used, strings[place - 1U], strlen(strings[place - 1U]));
			at += 3;
		}

		/* What did not fit is remembered; the rest is still written as far as it goes. */
		if (put != 0)
			error = ERANGE;
	}

	/* A place without its string is the caller's mistake. */
	if (refused)
		return EINVAL;

	/* A text that did not fit is cut, but still ends with its NUL. */
	if (error != 0)
		return error;

	/* Succeeded: the whole text is written. */
	return 0;
}

/*
 * Reads a program's catalogs in a language from a directory into state,
 * touching nothing else (any thread may call it).  Returns 0, or EINVAL,
 * ENOMEM or EFBIG with state holding nothing.
 */
static int
tr_state_read(
	struct tr_state *state,
	const char *directory,
	const char *domain,
	const char *language)
{
	int error;
	int differs;
	unsigned index;

	/* Nothing until it is read. */
	memset(state, 0, sizeof(*state));

	/* Only names that make a plain path. */
	error = EINVAL;
	if (directory != NULL)
		error = tr_name_valid(domain);
	if (error == 0)
		error = tr_name_valid(language);
	if (error != 0)
		return error;

	/* The names of the state. */
	(void)snprintf(state->language, sizeof(state->language), "%s", language);
	(void)snprintf(state->domain, sizeof(state->domain), "%s", domain);

	/*
	 * English is the source's own text: no catalog is read, and any other
	 * language reads the program's domain, then the shared one.
	 */
	differs = strcmp(language, "en");
	if (differs != 0) {
		/* The program's own catalog. */
		error = tr_catalog_read(&state->catalogs[0], directory, language, domain);

		/* The shared one, after it read. */
		if (error == 0)
			error = tr_catalog_read(&state->catalogs[1], directory, language, KL_TR_SHARED_DOMAIN);
	}

	/* A failure keeps nothing. */
	if (error != 0) {
		for (index = 0; index < TR_CATALOGS; index++)
			tr_catalog_free(&state->catalogs[index]);
		memset(state, 0, sizeof(*state));
		return error;
	}

	/* Succeeded: the state holds the language's catalogs. */
	return 0;
}

/*
 * Makes a state read by tr_state_read the one in use; the old catalogs go
 * (the texts they gave are no longer valid).
 */
static void
tr_state_use(
	const struct tr_state *state)
{
	/* The old catalogs, then the new ones in their place. */
	kl_tr_close();
	tr_state = *state;
}

/*
 * Tells whether a language's or a domain's name is lower-case letters,
 * digits, '-' and '_', and not too long: 0 or EINVAL.
 */
static int
tr_name_valid(
	const char *name)
{
	size_t length;
	size_t index;
	char character;

	/* A name is needed, short enough for the state. */
	if (name == NULL)
		return EINVAL;
	length = strlen(name);
	if (length == 0U || length > KL_TR_NAME_MAX)
		return EINVAL;

	/* Each character is one a file's name takes plainly. */
	for (index = 0; index < length; index++) {
		character = name[index];
		if (character >= 'a' && character <= 'z')
			continue;
		if (character >= '0' && character <= '9')
			continue;
		if (character == '-' || character == '_')
			continue;
		return EINVAL;
	}

	/* Succeeded: the name is plain. */
	return 0;
}

/*
 * Reads one catalog, DIRECTORY/LANGUAGE/DOMAIN.tr; a file that is not
 * there is an empty catalog.  Returns 0, ENOMEM, or EFBIG for a file
 * larger than the catalogs may be.
 */
static int
tr_catalog_read(
	struct tr_catalog *catalog,
	const char *directory,
	const char *language,
	const char *domain)
{
	char path[TR_PATH_MAX];
	struct stat status;
	ssize_t got;
	size_t done;
	int length;
	int descriptor;
	int error;
	int plain;

	/* The file's path, which must fit. */
	length = snprintf(path, sizeof(path), "%s/%s/%s.tr", directory, language, domain);
	if (length < 0 || (size_t)length >= sizeof(path))
		return ENAMETOOLONG;

	/* A catalog not there leaves the texts in English. */
	descriptor = open(path, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0)
		return 0;

	/* Its size; what is not a plain file is no catalog. */
	error = fstat(descriptor, &status);
	if (error != 0) {
		(void)close(descriptor);
		return 0;
	}

	/* A directory or a device is no catalog. */
	plain = S_ISREG(status.st_mode);
	if (!plain) {
		(void)close(descriptor);
		return 0;
	}

	/* A catalog larger than any should be is refused. */
	if ((size_t)status.st_size > TR_FILE_MAX) {
		(void)close(descriptor);
		return EFBIG;
	}

	/* Room for the whole file and a NUL after it. */
	catalog->text = malloc((size_t)status.st_size + 1U);
	if (catalog->text == NULL) {
		(void)close(descriptor);
		return ENOMEM;
	}

	/* The file, read until its end (a short read is retried). */
	done = 0;
	while (done < (size_t)status.st_size) {
		got = read(descriptor, catalog->text + done, (size_t)status.st_size - done);
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			break;
		done += (size_t)got;
	}

	/* The file is done with; the text ends with a NUL for the parser. */
	(void)close(descriptor);
	catalog->text[done] = '\0';

	/* The entries of the text. */
	error = tr_catalog_parse(catalog);
	if (error != 0) {
		tr_catalog_free(catalog);
		return error;
	}

	/* The table that finds them. */
	error = tr_catalog_index(catalog);
	if (error != 0) {
		tr_catalog_free(catalog);
		return error;
	}

	/* Succeeded: the catalog is read. */
	return 0;
}

/*
 * Cuts a catalog's text into its lines and their fields, and keeps each
 * entry.  Returns 0 or ENOMEM.
 */
static int
tr_catalog_parse(
	struct tr_catalog *catalog)
{
	char *fields[TR_FIELDS_MAX];
	char *line;
	char *end;
	size_t capacity;
	unsigned count;
	int error;

	/* Each line, its end replaced by a NUL. */
	capacity = 0;
	line = catalog->text;
	while (*line != '\0') {
		end = strchr(line, '\n');
		if (end != NULL)
			*end = '\0';

		/* A line's end of a CRLF file goes too. */
		if (end != NULL && end > line && end[-1] == '\r')
			end[-1] = '\0';

		/* A comment or an empty line has no entry. */
		if (line[0] != '#' && line[0] != '\0') {
			count = tr_split(line, fields, TR_FIELDS_MAX);
			error = tr_entry_add(catalog, fields, count, &capacity);
			if (error != 0)
				return error;
		}

		/* The next line, or the end of the text. */
		if (end == NULL)
			break;
		line = end + 1;
	}

	/* Succeeded: every entry is kept. */
	return 0;
}

/*
 * Keeps an entry from its fields, when they make one (the kind's word and
 * its fields, none empty); a line that does not is left out.  Returns 0 or
 * ENOMEM.
 */
static int
tr_entry_add(
	struct tr_catalog *catalog,
	char **fields,
	unsigned count,
	size_t *capacity)
{
	struct tr_entry entry;
	struct tr_entry *grown;
	char *next;
	size_t size;
	size_t length;
	unsigned index;
	int message;
	int context;
	int plural;

	/* Each field is decoded, and none may be empty. */
	for (index = 0; index < count; index++) {
		tr_unescape(fields[index]);
		if (fields[index][0] == '\0')
			return 0;
	}

	/* The kind's word. */
	message = strcmp(fields[0], "msg");
	context = strcmp(fields[0], "ctx");
	plural = strcmp(fields[0], "plural");

	/* The entry its kind's word and fields describe. */
	memset(&entry, 0, sizeof(entry));
	if (message == 0) {
		/* msg ENGLISH TEXT. */
		if (count != 3U)
			return 0;
		entry.kind = TR_KIND_MESSAGE;
		entry.english = fields[1];
		entry.forms = fields[2];
		entry.form_count = 1;
	} else if (context == 0) {
		/* ctx CONTEXT ENGLISH TEXT. */
		if (count != 4U)
			return 0;
		entry.kind = TR_KIND_CONTEXT;
		entry.context = fields[1];
		entry.english = fields[2];
		entry.forms = fields[3];
		entry.form_count = 1;
	} else if (plural == 0) {
		/* plural SINGULAR PLURAL FORM [FORM ...]. */
		if (count < 4U)
			return 0;
		entry.kind = TR_KIND_PLURAL;
		entry.english = fields[1];
		entry.plural = fields[2];
		entry.forms = fields[3];
		entry.form_count = count - 3U;

		/* The forms one after another, each ending with its NUL (decoding left gaps between them). */
		next = fields[3] + strlen(fields[3]) + 1U;
		for (index = 4; index < count; index++) {
			length = strlen(fields[index]) + 1U;
			memmove(next, fields[index], length);
			next += length;
		}
	} else {
		/* A word no entry has. */
		return 0;
	}

	/* The key's hash, which the table finds it by. */
	entry.hash = tr_hash(entry.kind, entry.context, entry.english, entry.plural);

	/* Room for one more entry, doubling. */
	if (catalog->count == *capacity) {
		size = *capacity * 2U;
		if (size == 0U)
			size = 64U;
		grown = realloc(catalog->entries, size * sizeof(*grown));
		if (grown == NULL)
			return ENOMEM;
		catalog->entries = grown;
		*capacity = size;
	}

	/* Succeeded: the entry is kept. */
	catalog->entries[catalog->count] = entry;
	catalog->count++;
	return 0;
}

/*
 * Builds the table of a catalog's entries.  Returns 0 or ENOMEM.
 */
static int
tr_catalog_index(
	struct tr_catalog *catalog)
{
	size_t mask;
	size_t slot;
	size_t index;

	/* An empty catalog needs no table. */
	if (catalog->count == 0U)
		return 0;

	/* At least twice as many slots as entries, a power of two. */
	catalog->slot_count = 16U;
	while (catalog->slot_count < catalog->count * 2U)
		catalog->slot_count *= 2U;
	catalog->slots = calloc(catalog->slot_count, sizeof(*catalog->slots));
	if (catalog->slots == NULL)
		return ENOMEM;

	/* Each entry in the first free slot from its hash (an entry repeated later wins: it is found first). */
	mask = catalog->slot_count - 1U;
	for (index = catalog->count; index > 0U; index--) {
		slot = catalog->entries[index - 1U].hash & mask;
		while (catalog->slots[slot] != 0U)
			slot = (slot + 1U) & mask;
		catalog->slots[slot] = (uint32_t)index;
	}

	/* Succeeded: the entries can be found. */
	return 0;
}

/*
 * Frees a catalog's memory and empties it.
 */
static void
tr_catalog_free(
	struct tr_catalog *catalog)
{
	/* Its table, its entries and its text. */
	free(catalog->slots);
	free(catalog->entries);
	free(catalog->text);
	memset(catalog, 0, sizeof(*catalog));
}

/*
 * Cuts a line at its TABs into at most most fields.  Returns the count; a
 * line with more fields keeps the rest in its last one.
 */
static unsigned
tr_split(
	char *line,
	char **fields,
	unsigned most)
{
	char *tab;
	unsigned count;

	/* Each field up to the next TAB. */
	count = 0;
	fields[count] = line;
	count++;
	for (;;) {
		if (count == most)
			break;
		tab = strchr(fields[count - 1U], '\t');
		if (tab == NULL)
			break;
		*tab = '\0';
		fields[count] = tab + 1;
		count++;
	}

	/* The fields of the line. */
	return count;
}

/*
 * Decodes a field's escapes in place: \t, \n and \\.  Another backslash
 * stays as it is.
 */
static void
tr_unescape(
	char *field)
{
	char *from;
	char *to;

	/* Each byte, a backslash and the letter after it made one byte. */
	from = field;
	to = field;
	while (*from != '\0') {
		if (from[0] == '\\' && from[1] == 't') {
			*to = '\t';
			from += 2;
		} else if (from[0] == '\\' && from[1] == 'n') {
			*to = '\n';
			from += 2;
		} else if (from[0] == '\\' && from[1] == '\\') {
			*to = '\\';
			from += 2;
		} else {
			*to = *from;
			from++;
		}

		/* One byte written. */
		to++;
	}

	/* The decoded field ends where its bytes do. */
	*to = '\0';
}

/*
 * Hashes an entry's key: its kind and its texts, each with a separator
 * after it so that the fields do not run into one another.
 */
static uint32_t
tr_hash(
	enum tr_kind kind,
	const char *context,
	const char *english,
	const char *plural)
{
	uint32_t hash;

	/* The kind first. */
	hash = TR_HASH_BASIS;
	hash ^= (uint32_t)kind + 1U;
	hash *= TR_HASH_PRIME;

	/* The texts the kind has. */
	if (context != NULL)
		hash = tr_hash_text(hash, context);
	hash = tr_hash_text(hash, english);
	if (plural != NULL)
		hash = tr_hash_text(hash, plural);

	/* The key's hash. */
	return hash;
}

/*
 * Adds a text and a separator to a hash.
 */
static uint32_t
tr_hash_text(
	uint32_t hash,
	const char *text)
{
	const unsigned char *at;

	/* Each byte, then the separator (0x1f, which a text of the interface does not hold). */
	for (at = (const unsigned char *)text; *at != '\0'; at++) {
		hash ^= *at;
		hash *= TR_HASH_PRIME;
	}

	/* The separator after the text. */
	hash ^= 0x1fU;
	hash *= TR_HASH_PRIME;

	/* The hash with the text. */
	return hash;
}

/*
 * Finds an entry in the program's catalogs, its own domain's first.
 */
static const struct tr_entry *
tr_find(
	enum tr_kind kind,
	const char *context,
	const char *english,
	const char *plural)
{
	const struct tr_entry *entry;
	uint32_t hash;
	unsigned index;

	/* English has no catalog to look in. */
	if (tr_state.catalogs[0].count == 0U && tr_state.catalogs[1].count == 0U)
		return NULL;

	/* Each catalog in order. */
	hash = tr_hash(kind, context, english, plural);
	for (index = 0; index < TR_CATALOGS; index++) {
		entry = tr_catalog_find(&tr_state.catalogs[index], kind, context, english, plural, hash);
		if (entry != NULL)
			return entry;
	}

	/* No catalog has it. */
	return NULL;
}

/*
 * Finds an entry in one catalog by its hash and key.
 */
static const struct tr_entry *
tr_catalog_find(
	const struct tr_catalog *catalog,
	enum tr_kind kind,
	const char *context,
	const char *english,
	const char *plural,
	uint32_t hash)
{
	const struct tr_entry *entry;
	size_t mask;
	size_t slot;
	int matches;

	/* An empty catalog has nothing. */
	if (catalog->slot_count == 0U)
		return NULL;

	/* The slots from the hash's own, until an empty one. */
	mask = catalog->slot_count - 1U;
	slot = hash & mask;
	while (catalog->slots[slot] != 0U) {
		entry = &catalog->entries[catalog->slots[slot] - 1U];

		/* The same key: the same hash and kind, then the same texts. */
		matches = 0;
		if (entry->hash == hash && entry->kind == kind)
			matches = tr_key_same(entry, context, english, plural);
		if (matches)
			return entry;

		/* The next slot, wrapping. */
		slot = (slot + 1U) & mask;
	}

	/* Not in this catalog. */
	return NULL;
}

/*
 * Tells whether an entry's texts are a key's: 1 or 0.
 */
static int
tr_key_same(
	const struct tr_entry *entry,
	const char *context,
	const char *english,
	const char *plural)
{
	int same;

	/* The context, the English and the plural, in turn. */
	same = tr_same(entry->context, context);
	if (!same)
		return 0;
	same = tr_same(entry->english, english);
	if (!same)
		return 0;
	same = tr_same(entry->plural, plural);
	if (!same)
		return 0;

	/* The same key. */
	return 1;
}

/*
 * Tells whether two texts that may be NULL are the same: 1 or 0.
 */
static int
tr_same(
	const char *left,
	const char *right)
{
	int differs;

	/* Both absent, or one absent. */
	if (left == NULL && right == NULL)
		return 1;
	if (left == NULL || right == NULL)
		return 0;

	/* The texts themselves. */
	differs = strcmp(left, right);
	if (differs != 0)
		return 0;

	/* The same text. */
	return 1;
}

/*
 * Reports which of a language's forms a number takes: Japanese has one,
 * English and a language not known here the singular for one and the
 * plural otherwise.
 */
static unsigned
tr_plural_form(
	const char *language,
	unsigned long count)
{
	int differs;

	/* Japanese: one form for every number. */
	differs = strcmp(language, "ja");
	if (differs == 0)
		return 0;

	/* English's rule. */
	if (count == 1UL)
		return 0;

	/* The plural. */
	return 1;
}

/*
 * Gives an entry's form by its number; an entry with fewer forms gives its
 * last.
 */
static const char *
tr_form(
	const struct tr_entry *entry,
	unsigned form)
{
	const char *text;
	unsigned index;

	/* The forms one after another, as far as the one asked or the last. */
	text = entry->forms;
	for (index = 0; index < form && index + 1U < entry->form_count; index++)
		text += strlen(text) + 1U;

	/* The form. */
	return text;
}

/*
 * Appends bytes to a text being written, as far as they fit with the NUL.
 * Returns 0, or ERANGE when some were left out.
 */
static int
tr_put(
	char *out,
	size_t size,
	size_t *used,
	const char *text,
	size_t length)
{
	size_t room;

	/* The room before the NUL. */
	room = size - 1U - *used;

	/* What does not fit is left out, the NUL kept. */
	if (length > room) {
		memcpy(out + *used, text, room);
		*used += room;
		out[*used] = '\0';
		return ERANGE;
	}

	/* Succeeded: the bytes are written. */
	memcpy(out + *used, text, length);
	*used += length;
	out[*used] = '\0';
	return 0;
}

/*
 * Reports the place a pattern's text starts with: N for {N} (1 to 9), 0
 * when it starts with anything else.
 */
static unsigned
tr_place(
	const char *at)
{
	/* An opening brace, a digit from 1 to 9 and a closing brace. */
	if (at[0] != '{')
		return 0;
	if (at[1] < '1' || at[1] > '9')
		return 0;
	if (at[2] != '}')
		return 0;

	/* The place's number. */
	return (unsigned)(at[1] - '0');
}
