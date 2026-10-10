/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Keeps the paired phone's reduced vCards apart from editable contacts.
 * The UI thread owns this copy and its sorted number index. A whole pass
 * may prune only entries absent from two credible consecutive readings.
 */

#include "phone.h"

#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define BOOK_MAX 5000U
#define BOOK_TELS 8U
#define BOOK_CARD_MAX 8192U
#define BOOK_PATH_MAX 2048U

/* One immutable imported contact; its strings live until replacement or close. */
struct ph_phonebook_entry {
	char address[18];
	char key[17];
	char *name;
	char *numbers[BOOK_TELS];
	size_t count;
};

/* One normalized number points into the entry array until the next rebuild. */
struct book_number {
	char *key;
	size_t entry;
};

/* The store directory selected by ph_store_open, empty while closed. */
static char book_root[BOOK_PATH_MAX];

/* Imported entries belong to the UI thread, with a hard shared storage bound. */
static struct ph_phonebook_entry *book_entries;

/* The occupied prefix of book_entries, reset when the store closes. */
static size_t book_count;

/* The sorted number index, replaced after import, removal or country changes. */
static struct book_number *book_numbers;

/* The initialized prefix of book_numbers, including its owned normalized strings. */
static size_t book_number_count;

/* The keys received in the current whole pass; failed passes cannot prune. */
static char (*book_received)[17];

/* The occupied prefix of book_received, deduplicated before pruning. */
static size_t book_received_count;

static int book_path(char *path, const char *directory, const char *name, const char *suffix);
static int book_directory(const char *address, char *path, size_t size);
static int book_key_valid(const char *key);
static void book_free_entry(struct ph_phonebook_entry *entry);
static void book_free_index(void);
static int book_parse(const char *address, const char *key, const char *card, struct ph_phonebook_entry *entry);
static char *book_unescape(const char *text, size_t length);
static int book_read(const char *path, char *card, size_t size);
static int book_write(const char *path, const char *text);
static int book_load_directory(const char *address);
static int book_replace(struct ph_phonebook_entry *entry);
static int book_number_compare(const void *left, const void *right);
static int book_prune(const char *address, unsigned capped);

/*
 * Loads imported phonebooks without adding any conversation rows.
 */
int
ph_phonebook_open(
	const char *root)
{
	char path[BOOK_PATH_MAX];
	char address[18];
	struct dirent *entry;
	DIR *directory;
	size_t i;
	int length;
	int error;

	/* Establishes ownership before discovering existing copies. */
	ph_phonebook_close();
	length = snprintf(book_root, sizeof(book_root), "%s/phonebook", root);
	if (length < 0 || (size_t)length >= sizeof(book_root))
		return ENAMETOOLONG;
	book_entries = calloc(BOOK_MAX, sizeof(book_entries[0]));
	if (book_entries == NULL)
		return ENOMEM;

	/* Creates the copy directory on first use. */
	error = mkdir(book_root, 0700);
	if (error != 0 && errno != EEXIST)
		return errno;
	directory = opendir(book_root);
	if (directory == NULL)
		return errno;

	/* Loads only directories with a Bluetooth address as their complete name. */
	error = 0;
	for (;;) {
		entry = readdir(directory);
		if (entry == NULL)
			break;
		length = (int)strlen(entry->d_name);
		if (length != 15)
			continue;
		length = strncmp(entry->d_name, "bt-", 3U);
		if (length != 0)
			continue;

		/* Restores the canonical colon-separated address. */
		for (i = 0U; i < 6U; i++) {
			address[i * 3U] = entry->d_name[3U + i * 2U];
			address[i * 3U + 1U] = entry->d_name[4U + i * 2U];
			address[i * 3U + 2U] = ':';
		}

		/* Terminates the restored address before validating its directory. */
		address[17] = '\0';
		error = book_directory(address, path, sizeof(path));
		if (error != 0) {
			error = 0;
			continue;
		}

		/* Loads only the validated phone directory. */
		error = book_load_directory(address);
		if (error != 0)
			break;
	}

	/* Releases the directory even when a copy failed to load. */
	(void)closedir(directory);
	if (error != 0)
		return error;

	/* Succeeded: names can now be resolved by normalized number. */
	error = ph_phonebook_reindex();
	if (error != 0)
		return error;
	return 0;
}

/*
 * Releases the imported entries and any unfinished pass.
 */
void
ph_phonebook_close(void)
{
	size_t i;

	/* Releases strings before their containing arrays. */
	book_free_index();
	for (i = 0U; i < book_count; i++)
		book_free_entry(&book_entries[i]);
	free(book_entries);
	book_entries = NULL;
	book_count = 0U;
	free(book_received);
	book_received = NULL;
	book_received_count = 0U;
	book_root[0] = '\0';
}

/*
 * Rebuilds the number index using the store's current country code.
 */
int
ph_phonebook_reindex(void)
{
	char key[PH_NUMBER_KEY_MAX];
	struct book_number *number;
	size_t i;
	size_t j;
	int error;

	/* Discards references into the previous entry arrangement. */
	book_free_index();
	if (book_count == 0U)
		return 0;
	book_numbers = calloc(book_count * BOOK_TELS, sizeof(book_numbers[0]));
	if (book_numbers == NULL)
		return ENOMEM;

	/* Normalizes each imported number independently. */
	for (i = 0U; i < book_count; i++) {
		for (j = 0U; j < book_entries[i].count; j++) {
			error = ph_number_key(book_entries[i].numbers[j], key, sizeof(key));
			if (error != 0)
				continue;
			number = &book_numbers[book_number_count];
			number->key = strdup(key);
			if (number->key == NULL) {
				book_free_index();
				return ENOMEM;
			}

			/* Publishes a normalized key only after its allocation succeeded. */
			number->entry = i;
			book_number_count++;
		}
	}

	/* Succeeded: lookup needs only a binary search. */
	qsort(book_numbers, book_number_count, sizeof(book_numbers[0]), book_number_compare);
	return 0;
}

/*
 * Looks up an imported name; the store separately gives local names priority.
 */
const char *
ph_phonebook_name(
	const char *number)
{
	char key[PH_NUMBER_KEY_MAX];
	size_t low;
	size_t high;
	size_t middle;
	int compared;
	int error;

	/* Rejects numbers that cannot be normalized. */
	error = ph_number_key(number, key, sizeof(key));
	if (error != 0)
		return NULL;

	/* Finds the first match, including duplicate numbers from several cards. */
	low = 0U;
	high = book_number_count;
	while (low < high) {
		middle = low + (high - low) / 2U;
		compared = strcmp(book_numbers[middle].key, key);
		if (compared < 0) {
			low = middle + 1U;
		} else {
			high = middle;
		}
	}

	/* No imported contact has this number. */
	if (low == book_number_count)
		return NULL;
	compared = strcmp(book_numbers[low].key, key);
	if (compared != 0)
		return NULL;

	/* Succeeded: the entry owns the returned string. */
	return book_entries[book_numbers[low].entry].name;
}

/*
 * Starts collecting keys for a new whole phonebook reading.
 */
int
ph_phonebook_begin(void)
{
	/* Replaces a failed or finished pass without advancing its missing set. */
	free(book_received);
	book_received_count = 0U;
	book_received = calloc(BOOK_MAX, sizeof(book_received[0]));
	if (book_received == NULL)
		return ENOMEM;

	/* Succeeded: a bounded pass can receive cards. */
	return 0;
}

/*
 * Persists one reduced vCard, keeping identical files' modification times.
 */
int
ph_phonebook_put(
	const char *address,
	const struct kl_phone_item *item)
{
	struct ph_phonebook_entry entry;
	char path[BOOK_PATH_MAX];
	char directory[BOOK_PATH_MAX];
	char old[BOOK_CARD_MAX];
	char card[BOOK_CARD_MAX];
	const char *end;
	size_t prefix;
	size_t i;
	int length;
	int valid;
	int same;
	int error;

	/* Accepts only complete reduced cards in a live pass. */
	valid = book_key_valid(item->key);
	if (!valid || item->text == NULL || book_received == NULL)
		return EINVAL;
	if (item->length > 6144U)
		return EFBIG;
	error = book_directory(address, directory, sizeof(directory));
	if (error != 0)
		return error;
	end = strstr(item->text, "END:VCARD");
	if (end == NULL)
		return EINVAL;
	prefix = (size_t)(end - item->text);
	length = snprintf(card, sizeof(card), "%.*sX-KEILAND-SOURCE:bt:%s\r\n%s", (int)prefix, item->text, address, end);
	if (length < 0 || (size_t)length >= sizeof(card))
		return EFBIG;
	error = book_parse(address, item->key, card, &entry);
	if (error != 0)
		return error;

	/* Enforces the cap before making a file that cannot be loaded later. */
	for (i = 0U; i < book_count; i++) {
		same = strcasecmp(book_entries[i].address, address);
		if (same != 0)
			continue;
		same = strcmp(book_entries[i].key, item->key);
		if (same == 0)
			break;
	}

	/* Rejects a new card before exceeding the allocated entry prefix. */
	if (i == book_count && book_count >= BOOK_MAX) {
		book_free_entry(&entry);
		return ENOSPC;
	}

	/* Writes only changed content, atomically within the copy's directory. */
	error = mkdir(directory, 0700);
	if (error != 0 && errno != EEXIST) {
		book_free_entry(&entry);
		return errno;
	}

	/* Names the card beneath the validated canonical directory. */
	error = book_path(path, directory, item->key, ".vcf");
	if (error != 0) {
		book_free_entry(&entry);
		return ENAMETOOLONG;
	}

	/* Compares persisted bytes before choosing whether to replace the file. */
	error = book_read(path, old, sizeof(old));
	same = 1;
	if (error == 0)
		same = strcmp(old, card);
	if (error != 0 && error != ENOENT) {
		book_free_entry(&entry);
		return error;
	}

	/* Replaces changed content while leaving identical files untouched. */
	if (same != 0) {
		error = book_write(path, card);
		if (error != 0) {
			book_free_entry(&entry);
			return error;
		}
	}

	/* Replaces the in-memory contact after its file is durable. */
	error = book_replace(&entry);
	if (error != 0) {
		book_free_entry(&entry);
		return error;
	}

	/* Counts each key once, even if a changing remote book repeats it. */
	for (i = 0U; i < book_received_count; i++) {
		same = strcmp(book_received[i], item->key);
		if (same == 0)
			return 0;
	}

	/* Refuses additional received keys beyond the whole-pass bound. */
	if (book_received_count >= BOOK_MAX)
		return ENOSPC;
	memcpy(book_received[book_received_count], item->key, 17U);
	book_received_count++;

	/* Succeeded: pruning can account for this contact. */
	return 0;
}

/*
 * Ends a pass, pruning only credible complete readings and rebuilding names.
 */
int
ph_phonebook_end(
	const char *address,
	int complete,
	unsigned capped)
{
	int error;
	int indexed;

	/* Failed passes leave the previous missing set untouched. */
	error = 0;
	if (complete && book_received != NULL)
		error = book_prune(address, capped);
	free(book_received);
	book_received = NULL;
	book_received_count = 0U;

	/* Refreshes lookup after any entries already persisted, even on failure. */
	indexed = ph_phonebook_reindex();
	ph_store_apply_phone_names();
	if (error != 0)
		return error;
	if (indexed != 0)
		return indexed;

	/* Succeeded: the caller may advance its mark only for a complete pass. */
	return 0;
}

/*
 * Removes phonebook copies only when a known link state requires it.
 * Calls and messages remain outside these directories and are preserved.
 */
int
ph_phonebook_link(
	const struct kl_phone_link *link)
{
	char directory[BOOK_PATH_MAX];
	char path[BOOK_PATH_MAX];
	char address[18];
	struct dirent *entry;
	DIR *folder;
	size_t i;
	size_t kept;
	int forget;
	int error;

	/* Evaluates the owner-aware decision independently for each existing copy. */
	i = 0U;
	while (i < book_count) {
		forget = ph_phonebook_forget(book_entries[i].address, link);
		if (!forget) {
			i++;
			continue;
		}

		/* Removes only this application's imported card files and missing set. */
		(void)snprintf(address, sizeof(address), "%s", book_entries[i].address);
		error = book_directory(address, directory, sizeof(directory));
		if (error != 0)
			return error;
		folder = opendir(directory);
		if (folder == NULL && errno != ENOENT)
			return errno;
		if (folder != NULL) {
			for (;;) {
				entry = readdir(folder);
				if (entry == NULL)
					break;
				if (entry->d_name[0] == '.')
					continue;
				error = book_path(path, directory, entry->d_name, "");
				if (error != 0) {
					(void)closedir(folder);
					return error;
				}

				/* Removes the complete imported filename. */
				error = unlink(path);
				if (error != 0) {
					error = errno;
					(void)closedir(folder);
					return error;
				}
			}

			/* Releases the directory after all imported files have been removed. */
			(void)closedir(folder);
		}

		/* Releases every in-memory entry from the forgotten phone. */
		kept = 0U;
		for (i = 0U; i < book_count; i++) {
			forget = strcasecmp(book_entries[i].address, address);
			if (forget == 0) {
				book_free_entry(&book_entries[i]);
			} else {
				book_entries[kept] = book_entries[i];
				kept++;
			}
		}

		/* Publishes the compact prefix before evaluating another phone copy. */
		book_count = kept;
		i = 0U;
	}

	/* Succeeded: retained conversation names are no longer labelled as imported. */
	error = ph_phonebook_reindex();
	ph_store_apply_phone_names();
	if (error != 0)
		return error;
	return 0;
}

/* Builds one bounded copy path before any filesystem operation. */
static int
book_path(
	char *path,
	const char *directory,
	const char *name,
	const char *suffix)
{
	size_t directory_length;
	size_t name_length;
	size_t suffix_length;

	/* Measures all components before copying a whole path. */
	directory_length = strlen(directory);
	name_length = strlen(name);
	suffix_length = strlen(suffix);
	if (directory_length + name_length + suffix_length + 2U > BOOK_PATH_MAX)
		return ENAMETOOLONG;

	/* Succeeded: includes the separator, suffix and terminator. */
	memcpy(path, directory, directory_length);
	path[directory_length] = '/';
	memcpy(path + directory_length + 1U, name, name_length);
	memcpy(path + directory_length + name_length + 1U, suffix, suffix_length + 1U);
	return 0;
}

/* Forms a canonical copy directory, accepting only a complete Bluetooth address. */
static int
book_directory(
	const char *address,
	char *path,
	size_t size)
{
	char compact[13];
	size_t i;
	size_t j;
	int length;
	int valid;

	/* Validates all address bytes before using any as a path component. */
	if (address == NULL)
		return EINVAL;
	i = strlen(address);
	if (i != 17U)
		return EINVAL;
	j = 0U;
	for (i = 0U; i < 17U; i++) {
		if (i % 3U == 2U) {
			if (address[i] != ':')
				return EINVAL;
			continue;
		}

		/* Keeps hexadecimal pairs in one stable upper-case directory name. */
		valid = 0;
		if (address[i] >= '0' && address[i] <= '9')
			valid = 1;
		if (address[i] >= 'a' && address[i] <= 'f')
			valid = 1;
		if (address[i] >= 'A' && address[i] <= 'F')
			valid = 1;
		if (!valid)
			return EINVAL;
		compact[j] = address[i];
		if (compact[j] >= 'a' && compact[j] <= 'f')
			compact[j] -= 'a' - 'A';
		j++;
	}

	/* Succeeded: a path beneath this store's phonebook directory. */
	compact[j] = '\0';
	length = snprintf(path, size, "%s/bt-%s", book_root, compact);
	if (length < 0 || (size_t)length >= size)
		return ENAMETOOLONG;
	return 0;
}

/* Accepts the fixed hexadecimal key emitted by the PBAP reducer. */
static int
book_key_valid(
	const char *key)
{
	size_t length;
	size_t span;

	/* A complete key, never a relative filename. */
	if (key == NULL)
		return 0;
	length = strlen(key);
	span = strspn(key, "0123456789abcdefABCDEF");
	if (length != 16U || span != length)
		return 0;

	/* Succeeded: a reducer key. */
	return 1;
}

/* Releases the strings owned by one imported card. */
static void
book_free_entry(
	struct ph_phonebook_entry *entry)
{
	size_t i;

	/* Releases independently allocated names and numbers. */
	free(entry->name);
	for (i = 0U; i < entry->count; i++)
		free(entry->numbers[i]);
	memset(entry, 0, sizeof(*entry));
}

/* Releases an index before its entries can move. */
static void
book_free_index(void)
{
	size_t i;

	/* Frees only the initialized prefix after partial allocation failures. */
	for (i = 0U; i < book_number_count; i++)
		free(book_numbers[i].key);
	free(book_numbers);
	book_numbers = NULL;
	book_number_count = 0U;
}

/* Parses the daemon's reduced vCard 3.0, whose FN and TEL lines are already unfolded. */
static int
book_parse(
	const char *address,
	const char *key,
	const char *card,
	struct ph_phonebook_entry *entry)
{
	const char *line;
	const char *end;
	size_t length;
	int same;

	/* Initializes ownership so a malformed card has one cleanup path. */
	memset(entry, 0, sizeof(*entry));
	(void)snprintf(entry->address, sizeof(entry->address), "%s", address);
	(void)snprintf(entry->key, sizeof(entry->key), "%s", key);
	line = card;

	/* Reads only reduced properties, preserving escaped punctuation in names. */
	while (*line != '\0') {
		length = strcspn(line, "\r\n");
		same = strncmp(line, "FN:", 3U);
		if (same == 0 && entry->name == NULL) {
			entry->name = book_unescape(line + 3U, length - 3U);
			if (entry->name == NULL) {
				book_free_entry(entry);
				return ENOMEM;
			}
		}

		/* Each TEL belongs to this contact, not to a new list row. */
		same = strncmp(line, "TEL:", 4U);
		if (same == 0 && entry->count < BOOK_TELS) {
			entry->numbers[entry->count] = book_unescape(line + 4U, length - 4U);
			if (entry->numbers[entry->count] == NULL) {
				book_free_entry(entry);
				return ENOMEM;
			}

			/* Makes the allocated number visible to parsing cleanup and indexing. */
			entry->count++;
		}

		/* Skips the complete CR/LF sequence before the next property. */
		end = line + length;
		while (*end == '\r' || *end == '\n')
			end++;
		line = end;
	}

	/* Reduced contacts require a usable name and at least one number. */
	if (entry->name == NULL || entry->count == 0U) {
		book_free_entry(entry);
		return EINVAL;
	}

	/* Succeeded: the entry owns all extracted strings. */
	return 0;
}

/* Decodes reduced vCard escapes into one allocated property string. */
static char *
book_unescape(
	const char *text,
	size_t length)
{
	char *copy;
	size_t i;
	size_t used;
	char c;

	/* Gives the decoded string a maximum of the encoded property's length. */
	copy = malloc(length + 1U);
	if (copy == NULL)
		return NULL;

	/* Decodes escaped line endings and punctuation without copying control bytes. */
	used = 0U;
	for (i = 0U; i < length; i++) {
		c = text[i];
		if (c == '\\' && i + 1U < length) {
			i++;
			c = text[i];
			if (c == 'n' || c == 'N')
				c = ' ';
		}

		/* Keeps imported names on one display line. */
		if ((unsigned char)c < 0x20U)
			c = ' ';
		copy[used] = c;
		used++;
	}

	/* Succeeded: the caller owns a NUL-terminated property. */
	copy[used] = '\0';
	return copy;
}

/* Reads a bounded complete card or missing-key file. */
static int
book_read(
	const char *path,
	char *card,
	size_t size)
{
	FILE *file;
	size_t length;
	int failed;
	int close_error;

	/* Opens only an existing file; absence is handled by the caller. */
	file = fopen(path, "rb");
	if (file == NULL)
		return errno;
	length = fread(card, 1U, size, file);
	failed = ferror(file);
	close_error = fclose(file);
	if (failed || close_error != 0)
		return EIO;
	if (length == size)
		return EFBIG;

	/* Succeeded: no truncated card is accepted as a whole reading. */
	card[length] = '\0';
	return 0;
}

/* Atomically replaces a changed file within its own directory. */
static int
book_write(
	const char *path,
	const char *text)
{
	char temporary[BOOK_PATH_MAX];
	FILE *file;
	size_t length;
	size_t written;
	int error;
	int closed;

	/* Uses a sibling so rename cannot cross file systems. */
	error = snprintf(temporary, sizeof(temporary), "%s.tmp", path);
	if (error < 0 || (size_t)error >= sizeof(temporary))
		return ENAMETOOLONG;
	file = fopen(temporary, "wb");
	if (file == NULL)
		return errno;
	length = strlen(text);
	written = fwrite(text, 1U, length, file);
	closed = fclose(file);
	if (written != length || closed != 0)
		return EIO;
	error = rename(temporary, path);
	if (error != 0)
		return errno;

	/* Succeeded: readers see either complete version. */
	return 0;
}

/* Loads valid card filenames from one phone's copy directory. */
static int
book_load_directory(
	const char *address)
{
	struct ph_phonebook_entry made;
	struct dirent *entry;
	char directory[BOOK_PATH_MAX];
	char path[BOOK_PATH_MAX];
	char key[17];
	char card[BOOK_CARD_MAX];
	DIR *folder;
	size_t length;
	int valid;
	int same;
	int error;

	/* Opens the validated directory without trusting arbitrary path components. */
	error = book_directory(address, directory, sizeof(directory));
	if (error != 0)
		return error;
	folder = opendir(directory);
	if (folder == NULL)
		return errno;

	/* Skips unrelated files, malformed copies, and entries beyond the bound. */
	error = 0;
	for (;;) {
		entry = readdir(folder);
		if (entry == NULL)
			break;
		length = strlen(entry->d_name);
		if (length != 20U)
			continue;
		same = strcmp(entry->d_name + 16U, ".vcf");
		if (same != 0)
			continue;
		memcpy(key, entry->d_name, 16U);
		key[16] = '\0';
		valid = book_key_valid(key);
		if (!valid || book_count >= BOOK_MAX)
			continue;
		error = book_path(path, directory, entry->d_name, "");
		if (error != 0)
			break;

		/* Reads only a complete bounded card path. */
		error = book_read(path, card, sizeof(card));
		if (error != 0) {
			ph_log("PHONEBOOK unreadable error=%d", error);
			error = 0;
			continue;
		}

		/* Rejects malformed saved cards without admitting partial contacts. */
		error = book_parse(address, key, card, &made);
		if (error == EINVAL) {
			error = 0;
			continue;
		}

		/* Stops loading on allocation errors rather than claiming a complete copy. */
		if (error != 0)
			break;
		error = book_replace(&made);
		if (error != 0) {
			book_free_entry(&made);
			break;
		}
	}

	/* Succeeded: retained entries are ready for indexing. */
	(void)closedir(folder);
	if (error != 0)
		return error;
	return 0;
}

/* Transfers a parsed card into the bounded entry array, replacing its own prior version. */
static int
book_replace(
	struct ph_phonebook_entry *entry)
{
	size_t i;
	int same;

	/* Finds the same phone's stable card key. */
	for (i = 0U; i < book_count; i++) {
		same = strcasecmp(book_entries[i].address, entry->address);
		if (same != 0)
			continue;
		same = strcmp(book_entries[i].key, entry->key);
		if (same == 0)
			break;
	}

	/* Extends only the occupied prefix, never beyond its allocation. */
	if (i == book_count) {
		if (book_count >= BOOK_MAX)
			return ENOSPC;
		book_count++;
	} else {
		book_free_entry(&book_entries[i]);
	}

	/* Succeeded: the array now owns the parsed strings. */
	book_entries[i] = *entry;
	memset(entry, 0, sizeof(*entry));
	return 0;
}

/* Orders numbers and breaks duplicate-number ties by stable phone and card identity. */
static int
book_number_compare(
	const void *left,
	const void *right)
{
	const struct book_number *a;
	const struct book_number *b;
	int same;

	/* Orders normalized numbers first. */
	a = left;
	b = right;
	same = strcmp(a->key, b->key);
	if (same != 0)
		return same;
	same = strcasecmp(book_entries[a->entry].address, book_entries[b->entry].address);
	if (same != 0)
		return same;

	/* Succeeded: duplicate numbers always choose the same card. */
	same = strcmp(book_entries[a->entry].key, book_entries[b->entry].key);
	return same;
}

/* Applies the pure two-pass deletion plan and persists its next missing-key set. */
static int
book_prune(
	const char *address,
	unsigned capped)
{
	const char **current;
	const char **received;
	const char **missing;
	unsigned char *remove;
	unsigned char *next;
	char directory[BOOK_PATH_MAX];
	char path[BOOK_PATH_MAX];
	char *text;
	char *previous;
	char *line;
	char *end;
	size_t current_count;
	size_t missing_count;
	size_t i;
	size_t kept;
	size_t used;
	int counts;
	int same;
	int error;

	/* Allocates one workspace whose slices have fixed bounds. */
	text = calloc(1U, BOOK_MAX * (sizeof(*current) * 3U + 2U + 36U));
	if (text == NULL)
		return ENOMEM;
	current = (const char **)text;
	received = current + BOOK_MAX;
	missing = received + BOOK_MAX;
	remove = (unsigned char *)(missing + BOOK_MAX);
	next = remove + BOOK_MAX;
	previous = (char *)(next + BOOK_MAX);
	text = previous + BOOK_MAX * 18U;

	/* Selects this phone's copy and the deduplicated received keys. */
	current_count = 0U;
	for (i = 0U; i < book_count; i++) {
		same = strcasecmp(book_entries[i].address, address);
		if (same == 0) {
			current[current_count] = book_entries[i].key;
			current_count++;
		}
	}

	/* Builds the received-key view used by the pure pruning decision. */
	for (i = 0U; i < book_received_count; i++)
		received[i] = book_received[i];
	ph_phonebook_sort_keys(current, current_count);
	ph_phonebook_sort_keys(received, book_received_count);

	/* Reads only the previous credible pass's missing keys. */
	error = book_directory(address, directory, sizeof(directory));
	if (error != 0) {
		free(current);
		return error;
	}

	/* Selects the prior pass ledger from the same phone directory. */
	error = book_path(path, directory, "missing.txt", "");
	if (error != 0) {
		free(current);
		return error;
	}

	/* Uses the ledger belonging to this phone copy. */
	error = book_read(path, previous, BOOK_MAX * 18U);
	if (error != 0 && error != ENOENT) {
		free(current);
		return error;
	}

	/* Treats a missing ledger as the first credible pass. */
	if (error == ENOENT)
		previous[0] = '\0';
	missing_count = 0U;
	line = previous;
	while (*line != '\0' && missing_count < BOOK_MAX) {
		end = strchr(line, '\n');
		if (end == NULL)
			break;
		*end = '\0';
		same = book_key_valid(line);
		if (same) {
			missing[missing_count] = line;
			missing_count++;
		}

		/* Moves past the terminated ledger key to the next complete line. */
		line = end + 1U;
	}

	/* Orders the prior keys before testing two consecutive absences. */
	ph_phonebook_sort_keys(missing, missing_count);
	counts = ph_phonebook_prune_plan(current, current_count, received, book_received_count, missing, missing_count, 1, capped, remove, next);
	if (!counts) {
		free(current);
		return 0;
	}

	/* Deletes twice-missing cards before changing the entry arrangement. */
	used = 0U;
	for (i = 0U; i < current_count; i++) {
		if (next[i]) {
			(void)snprintf(text + used, 18U, "%s\n", current[i]);
			used += 17U;
		}

		/* Leaves present and only-once-missing cards in place. */
		if (!remove[i])
			continue;
		error = book_path(path, directory, current[i], ".vcf");
		if (error != 0) {
			free(current);
			return error;
		}

		/* Removes the twice-missing card from disk. */
		error = unlink(path);
		if (error != 0 && errno != ENOENT) {
			error = errno;
			free(current);
			return error;
		}
	}

	/* Persists the next missing set before publishing a successful pass. */
	error = book_path(path, directory, "missing.txt", "");
	if (error != 0) {
		free(current);
		return error;
	}

	/* Uses the ledger belonging to this phone copy. */
	error = book_write(path, text);
	if (error != 0) {
		free(current);
		return error;
	}

	/* Drops removed entries only after all key pointers have been consumed. */
	free(current);
	kept = 0U;
	for (i = 0U; i < book_count; i++) {
		same = strcasecmp(book_entries[i].address, address);
		if (same == 0) {
			error = book_path(path, directory, book_entries[i].key, ".vcf");
			if (error != 0)
				return error;

			/* Retains entries that still have their persisted card. */
			error = access(path, F_OK);
			if (error != 0 && errno == ENOENT) {
				book_free_entry(&book_entries[i]);
				continue;
			}
		}

		/* Transfers retained entry ownership into the compact array prefix. */
		book_entries[kept] = book_entries[i];
		kept++;
	}

	/* Succeeded: the caller can rebuild a compact number index. */
	book_count = kept;
	return 0;
}
