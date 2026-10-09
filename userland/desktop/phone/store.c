/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Phone's contacts and timelines on the disk (WS170 p002, plan/ws170/
 * phase001/phase.md section 1), under a root (~/Documents/Phone):
 *
 *   contacts/<id>.vcf          a contact: vCard 3.0's FN and TEL
 *   messages/<id>/<name>.txt   one item of its timeline: "Key: value"
 *                              lines (Kind, Channel, Direction, Date,
 *                              State, Detail; Source, Partial, Truncated
 *                              and Name for the paired phone's), an empty
 *                              line, the words
 *   sync/bt-<address>.state    how far the paired phone's messages were
 *                              brought in (messages_since, deep_at)
 *
 * The paired phone's messages (ws197-p004b, plan/ws197/phase004/phase.md
 * sections 2 and 6.3) go to the conversation of their number's key: the
 * contact whose number has that key, else a folder of the number alone
 * (messages/n<digits>, or messages/a<the sender's bytes in hexadecimal>),
 * read as the contact's once one is made.  A message with its key is kept
 * once, as s<key>.txt; one of this program's own sent texts, or one whose
 * time was not known, takes the key of the phone's copy when they match.
 * Header lines this program does not know are written back as they were.
 *
 * One file for each thing, so that a folder synchronized with the cloud
 * rarely has two machines write the same file; a change rewrites one
 * file, written beside it and renamed over it.  Everything is read when
 * the store opens: the contacts with the latest item first, each
 * timeline oldest first.  Only the window's thread touches the store.
 */

#include "phone.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* The longest path, line and item file read, with its NUL. */
#define STORE_PATH_MAX		2048U
#define STORE_LINE_MAX		1024U
#define STORE_ROOT_MAX		512U
#define STORE_ITEM_MAX		65536U

/* The most contacts. */
#define STORE_CONTACTS_MAX	1024U

/* How far apart one's own text and the phone's copy of it may be (seconds), and the longest key and country code with their NULs. */
#define STORE_MATCH_SECONDS	600
#define STORE_KEY_MAX		17U
#define STORE_COUNTRY_MAX	8U

/* How many seconds a day has. */
#define STORE_DAY_SECONDS	86400L

/* The words of the kinds, channels and states in the files, in their enums' orders. */
static const char *const store_kinds[] = { "text", "call", "photo", "file" };
static const char *const store_channels[] = { "sms", "mms", "rcs", "line", "voip" };
static const char *const store_channel_words[] = { "SMS", "MMS", "RCS", "Phone", "VoIP" };
static const char *const store_states[PH_STATES] = { "", "unread", "read", "sending", "sent", "delivered", "failed", "answered", "missed", "no-answer", "unknown" };

/* The days and months as words. */
static const char *const store_days[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const store_months[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

/* The contacts' colors, chosen by their names. */
static const kl_color store_colors[8] = {
	KL_RGB(0xf2994a), KL_RGB(0x56ccf2), KL_RGB(0xeb5757), KL_RGB(0x6fcf97),
	KL_RGB(0xbb6bd9), KL_RGB(0x2d9cdb), KL_RGB(0x828282), KL_RGB(0xf2c94c)
};

/* The root folder (empty while the store is closed). */
static char store_root[STORE_ROOT_MAX];

/* The contacts (allocated), how many, and the room. */
static struct ph_contact *store_contacts;
static size_t store_contact_count;
static size_t store_contact_capacity;

/* The number in the next file's name, so that two made in one second differ. */
static unsigned long store_serial;

/* The number naming the next item kept, for this run (ws197-p004b). */
static unsigned long store_item_serial;

/* The country code a number written without one belongs to (ws197-p004 section 0, P8). */
static char store_country[STORE_COUNTRY_MAX] = PH_COUNTRY_DEFAULT;

static int store_load_contacts(void);
static int store_load_contact(const char *id);
static int store_load_conversations(void);
static int store_load_items(long contact, const char *folder);
static void store_sort_items(long contact);
static int store_load_item(long contact, const char *path);
static int store_extra_add(char **extra, size_t *length, const char *key, const char *value);
static int store_append_contact(const char *id, const char *name, const char *number);
static int store_append_item(long contact, const struct ph_item *item);
static int store_write_item(const struct ph_item *item, const char *path);
static int store_put(char *text, size_t *used, const char *format, ...);
static int store_write_file(const char *path, const char *text, size_t length);
static int store_word(const char *const *words, size_t count, const char *word);
static int store_item_field(struct ph_item *item, const char *key, char *value);
static int store_key_folder(const char *key, char *folder, size_t size);
static int store_folder_key(const char *folder, char *key, size_t size);
static int store_key_valid(const char *key);
static int store_find_key(const char *key, long *contact, size_t *item);
static int store_candidate(long contact, const struct ph_phone_message *message, int with_source, size_t *item);
static int store_same_words(const char *kept, const char *came);
static int store_overlay(long contact, size_t item, const struct ph_phone_message *message, size_t *placed);
static int store_new_message(long contact, const struct ph_phone_message *message, int partial, size_t *placed);
static size_t store_place_item(long contact, size_t item);
static void store_count_unread(long contact);
static void store_clean_line(const char *text, char *clean, size_t size);
static int store_sync_path(const char *address, char *path, size_t size);
static void store_words(struct ph_item *item, time_t now);
static void store_initials(const char *name, char *initials, size_t size);
static kl_color store_color(const char *name);
static void store_sort_contacts(void);
static time_t store_latest(const struct ph_contact *contact);
static void store_free_item(struct ph_item *item);
static char *store_copy(const char *text, int *failed);
static const char *store_direction(int outgoing);
static int store_is_named(const char *name, const char *suffix);

/*
 * Opens the store at a root folder (made when it is not there) and reads
 * every contact and item.  Returns 0 or an errno value.
 */
int
ph_store_open(
	const char *root)
{
	char path[STORE_PATH_MAX];
	int error;

	/* The folders. */
	ph_store_close();
	(void)snprintf(store_root, sizeof(store_root), "%s", root);
	(void)mkdir(store_root, 0700);
	(void)snprintf(path, sizeof(path), "%s/contacts", store_root);
	(void)mkdir(path, 0700);
	(void)snprintf(path, sizeof(path), "%s/messages", store_root);
	(void)mkdir(path, 0700);
	(void)snprintf(path, sizeof(path), "%s/sync", store_root);
	(void)mkdir(path, 0700);

	/* Everything in them: the contacts, then the numbers' conversations (ws197-p004b). */
	error = store_load_contacts();
	if (error != 0)
		return error;
	error = store_load_conversations();
	if (error != 0)
		return error;

	/* Succeeded: the latest contact first. */
	store_sort_contacts();
	return 0;
}

/*
 * Frees everything the store holds.
 */
void
ph_store_close(void)
{
	size_t contact;
	size_t item;

	/* Each contact and its items. */
	for (contact = 0; contact < store_contact_count; contact++) {
		for (item = 0; item < store_contacts[contact].item_count; item++)
			store_free_item(&store_contacts[contact].items[item]);
		free(store_contacts[contact].items);
		free(store_contacts[contact].id);
		free(store_contacts[contact].name);
		free(store_contacts[contact].number);
	}

	/* The array. */
	free(store_contacts);
	store_contacts = NULL;
	store_contact_count = 0;
	store_contact_capacity = 0;
	store_root[0] = '\0';
}

/*
 * Reports the contacts and how many there are.
 */
const struct ph_contact *
ph_contacts(
	size_t *count)
{
	/* The contacts read and added. */
	*count = store_contact_count;
	return store_contacts;
}

/*
 * Adds a contact, its file written; *index is where it is.  A number's
 * conversation of the same number becomes the contact (ws197-p004b: its
 * folder is read as the contact's).  Returns 0, EINVAL for an empty
 * number, or an errno value.
 */
int
ph_store_add_contact(
	const char *name,
	const char *number,
	long *index)
{
	struct ph_contact *kept;
	char id[64];
	char path[STORE_PATH_MAX];
	char text[STORE_LINE_MAX * 2U];
	char *new_id;
	char *new_name;
	char *new_number;
	long conversation;
	int length;
	int failed;
	int error;

	/* A number is needed; the name may be the number. */
	if (number[0] == '\0')
		return EINVAL;
	if (name[0] == '\0')
		name = number;
	conversation = ph_store_conversation(number, NULL, 0);
	if (conversation >= 0 && !store_contacts[conversation].conversation)
		conversation = -1;

	/* Its ID and file. */
	store_serial++;
	(void)snprintf(id, sizeof(id), "c%lld-%lu", (long long)time(NULL), store_serial);
	(void)snprintf(path, sizeof(path), "%s/contacts/%s.vcf", store_root, id);
	length = snprintf(text, sizeof(text), "BEGIN:VCARD\r\nVERSION:3.0\r\nFN:%s\r\nTEL:%s\r\nEND:VCARD\r\n", name, number);
	if (length < 0 || (size_t)length >= sizeof(text))
		return E2BIG;
	error = store_write_file(path, text, (size_t)length);
	if (error != 0)
		return error;

	/* Its folder of items. */
	(void)snprintf(path, sizeof(path), "%s/messages/%s", store_root, id);
	(void)mkdir(path, 0700);

	/* The number's conversation becomes the contact, its items where they are. */
	if (conversation >= 0) {
		failed = 0;
		new_id = store_copy(id, &failed);
		new_name = store_copy(name, &failed);
		new_number = store_copy(number, &failed);
		if (failed) {
			free(new_id);
			free(new_name);
			free(new_number);
			return ENOMEM;
		}

		/* Its names and picture. */
		kept = &store_contacts[conversation];
		free(kept->id);
		free(kept->name);
		free(kept->number);
		kept->id = new_id;
		kept->name = new_name;
		kept->number = new_number;
		kept->conversation = 0;
		store_initials(name, kept->initials, sizeof(kept->initials));
		kept->color = store_color(name);
		*index = conversation;
		return 0;
	}

	/* Kept at the end. */
	error = store_append_contact(id, name, number);
	if (error != 0)
		return error;

	/* Succeeded: the contact is the last. */
	*index = (long)store_contact_count - 1L;
	return 0;
}

/*
 * Finds the contact or conversation of a number (their number keys
 * compared, ws197-p004 section 0); its index, or -1.
 */
long
ph_store_find_number(
	const char *number)
{
	long found;

	/* The contact, never made. */
	found = ph_store_conversation(number, NULL, 0);
	if (found < 0)
		return -1;

	/* Succeeded: its index. */
	return found;
}

/*
 * Adds an item to a contact's timeline, its file written; *item is where
 * it is in the timeline.  Returns 0 or an errno value.
 */
int
ph_store_add_item(
	long contact,
	enum ph_kind kind,
	enum ph_channel channel,
	int outgoing,
	time_t date,
	enum ph_state state,
	const char *text,
	const char *detail,
	size_t *item)
{
	struct ph_item made;
	char path[STORE_PATH_MAX];
	int error;

	/* A contact that is there. */
	if (contact < 0 || (size_t)contact >= store_contact_count)
		return EINVAL;

	/* Its file's name: the time, the number of this run, and the direction. */
	store_serial++;
	(void)snprintf(path, sizeof(path), "%s/messages/%s/%lld-%lu-%s.txt", store_root, store_contacts[contact].id, (long long)date, store_serial, store_direction(outgoing));

	/* The item, written. */
	memset(&made, 0, sizeof(made));
	made.kind = kind;
	made.channel = channel;
	made.outgoing = outgoing;
	made.date = date;
	made.state = state;
	made.text = (char *)text;
	made.detail = (char *)detail;
	made.path = path;
	error = store_write_item(&made, path);
	if (error != 0)
		return error;

	/* Kept at the end of the timeline. */
	error = store_append_item(contact, &made);
	if (error != 0)
		return error;

	/* Succeeded: the item is the last. */
	*item = store_contacts[contact].item_count - 1U;
	return 0;
}

/*
 * Changes an item's state (and detail, unless NULL), its file written
 * again.
 */
int
ph_store_set_state(
	long contact,
	size_t item,
	enum ph_state state,
	const char *detail)
{
	struct ph_item *kept;
	char *copied;
	int failed;
	int error;

	/* An item that is there. */
	if (contact < 0 || (size_t)contact >= store_contact_count)
		return EINVAL;
	if (item >= store_contacts[contact].item_count)
		return EINVAL;
	kept = &store_contacts[contact].items[item];

	/* The new state and detail. */
	kept->state = state;
	if (detail != NULL) {
		failed = 0;
		copied = store_copy(detail, &failed);
		if (failed)
			return ENOMEM;
		free(kept->detail);
		kept->detail = copied;
	}

	/* Its file, written again. */
	error = store_write_item(kept, kept->path);
	if (error != 0)
		return error;

	/* The words shown. */
	store_words(kept, time(NULL));
	return 0;
}

/*
 * Marks a contact's unread messages read (their files written again).
 */
int
ph_store_mark_read(
	long contact)
{
	struct ph_contact *kept;
	size_t index;
	int error;

	/* A contact that is there. */
	if (contact < 0 || (size_t)contact >= store_contact_count)
		return EINVAL;
	kept = &store_contacts[contact];

	/* Each unread item. */
	for (index = 0; index < kept->item_count; index++) {
		if (kept->items[index].state != PH_STATE_UNREAD)
			continue;
		error = ph_store_set_state(contact, index, PH_STATE_READ, NULL);
		if (error != 0)
			return error;
	}

	/* Succeeded: nothing unread. */
	kept->unread = 0;
	return 0;
}

/*
 * Reports a channel's name as the view shows it.
 */
const char *
ph_channel_word(
	enum ph_channel channel)
{
	/* A channel not known. */
	if ((unsigned)channel >= sizeof(store_channel_words) / sizeof(store_channel_words[0]))
		return "";

	/* Its name. */
	return store_channel_words[channel];
}

/*
 * Sets the country code a number written without one belongs to (digits,
 * ws197-p004 section 0, P8: the region's; PH_COUNTRY_DEFAULT until one is
 * set).  A code that is not 1 to 4 digits leaves it as it is.
 */
void
ph_store_set_country(
	const char *code)
{
	size_t length;
	size_t span;

	/* One to four digits. */
	length = strlen(code);
	span = strspn(code, "0123456789");
	if (length == 0U || length > 4U || span != length)
		return;

	/* Kept. */
	(void)snprintf(store_country, sizeof(store_country), "%s", code);
}

/*
 * Makes a number's key (ws197-p004 section 0): a sender with letters, or a
 * number of fewer than six digits, is its words trimmed and in lower case;
 * another number is "+", its country code and its digits.  Returns 0, or
 * EINVAL for nothing or no room.
 */
int
ph_number_key(
	const char *number,
	char *key,
	size_t size)
{
	char stripped[PH_NUMBER_KEY_MAX];
	const char *separator;
	const char *start;
	const char *rest;
	size_t length;
	size_t used;
	size_t index;
	size_t span;
	unsigned digits;
	int letters;
	int written;
	int alpha;

	/* Its digits and whether it has letters (any byte of a character beyond ASCII counts as one). */
	digits = 0U;
	letters = 0;
	for (index = 0U; number[index] != '\0'; index++) {
		alpha = isalpha((unsigned char)number[index]);
		if (number[index] >= '0' && number[index] <= '9')
			digits++;
		else if (alpha || (unsigned char)number[index] >= 0x80U)
			letters = 1;
	}

	/* The separators out; anything else but the digits and a leading "+" makes it words. */
	used = 0U;
	for (index = 0U; number[index] != '\0' && used + 1U < sizeof(stripped); index++) {
		separator = strchr(" -.()", number[index]);
		if (separator != NULL)
			continue;
		stripped[used] = number[index];
		used++;
	}

	/* The end of what is left, and whether it is the digits after a "+" or none. */
	stripped[used] = '\0';
	rest = stripped;
	if (rest[0] == '+')
		rest++;
	span = strspn(rest, "0123456789");
	length = strlen(rest);
	if (span != length)
		letters = 1;

	/* Words: trimmed and in lower case. */
	if (letters || digits < 6U) {
		start = number;
		while (*start == ' ' || *start == '\t')
			start++;
		length = strlen(start);
		while (length > 0U && (start[length - 1U] == ' ' || start[length - 1U] == '\t'))
			length--;
		if (length == 0U || length >= size)
			return EINVAL;
		for (index = 0U; index < length; index++)
			key[index] = (char)tolower((unsigned char)start[index]);
		key[length] = '\0';
		return 0;
	}

	/* A number: "+" kept, "00" is "+", one "0" or none takes the country code. */
	if (stripped[0] == '+') {
		written = snprintf(key, size, "+%s", stripped + 1);
	} else if (stripped[0] == '0' && stripped[1] == '0') {
		written = snprintf(key, size, "+%s", stripped + 2);
	} else if (stripped[0] == '0') {
		written = snprintf(key, size, "+%s%s", store_country, stripped + 1);
	} else {
		written = snprintf(key, size, "+%s%s", store_country, stripped);
	}

	/* No room. */
	if (written < 0 || (size_t)written >= size)
		return EINVAL;

	/* Succeeded: the key. */
	return 0;
}

/*
 * Finds the conversation of a number (ws197-p004b): the contact whose
 * number has its key, or the number's conversation of its own, made (its
 * folder too) when create is 1 and there is none, named by name (the
 * number when empty or NULL).  Returns its index, or -1.
 */
long
ph_store_conversation(
	const char *number,
	const char *name,
	int create)
{
	char key[PH_NUMBER_KEY_MAX];
	char other[PH_NUMBER_KEY_MAX];
	char folder[PH_NUMBER_KEY_MAX * 2U + 2U];
	char path[STORE_PATH_MAX];
	const char *shown;
	size_t index;
	int error;
	int same;

	/* The number's key and folder. */
	error = ph_number_key(number, key, sizeof(key));
	if (error != 0)
		return -1;
	error = store_key_folder(key, folder, sizeof(folder));
	if (error != 0)
		return -1;

	/* A contact of that number, or the number's own conversation. */
	for (index = 0U; index < store_contact_count; index++) {
		if (store_contacts[index].conversation) {
			same = strcmp(store_contacts[index].id, folder);
			if (same == 0)
				return (long)index;
			continue;
		}

		/* A contact's number by its key. */
		error = ph_number_key(store_contacts[index].number, other, sizeof(other));
		if (error != 0)
			continue;
		same = strcmp(other, key);
		if (same == 0)
			return (long)index;
	}

	/* None, and none to be made. */
	if (!create)
		return -1;

	/* The folder and the conversation, named as the phone names the other side. */
	(void)snprintf(path, sizeof(path), "%s/messages/%s", store_root, folder);
	(void)mkdir(path, 0700);
	shown = number;
	if (name != NULL && name[0] != '\0')
		shown = name;
	error = store_append_contact(folder, shown, number);
	if (error != 0)
		return -1;
	store_contacts[store_contact_count - 1U].conversation = 1;

	/* Succeeded: the new conversation. */
	return (long)store_contact_count - 1L;
}

/*
 * Keeps one message of the paired phone (ws197-p004 section 6.3): one the
 * store has by its key changes only to read when the phone read it; one
 * with a key matches one of the store's own without a source (the same
 * number, direction and words, within ten minutes), which takes the key;
 * else it is a new file s<key>.txt.  A message without a key (its time
 * not known) is kept unless one like it is there.  *contact and *item are
 * where it is, *merge what was done (PH_MERGE_*).  Returns 0, EINVAL, or
 * an errno value.
 */
int
ph_store_phone_message(
	const struct ph_phone_message *message,
	long *contact,
	size_t *item,
	int *merge)
{
	struct ph_item *kept;
	size_t found;
	size_t placed;
	long conversation;
	int partial;
	int matched;
	int same;
	int error;

	/* A message from a number, with words, and a key that names a file. */
	if (message->peer == NULL || message->peer[0] == '\0' || message->text == NULL || message->key == NULL)
		return EINVAL;
	same = strcmp(message->key, "-");
	partial = 0;
	if (same == 0)
		partial = 1;
	if (!partial) {
		matched = store_key_valid(message->key);
		if (!matched)
			return EINVAL;
	}

	/* Step 1: a message the store has by its key; read when the phone read it. */
	if (!partial) {
		matched = store_find_key(message->key, contact, item);
		if (matched) {
			kept = &store_contacts[*contact].items[*item];
			if (!message->outgoing && message->read && kept->state == PH_STATE_UNREAD) {
				error = ph_store_set_state(*contact, *item, PH_STATE_READ, NULL);
				if (error != 0)
					return error;
				store_count_unread(*contact);
			}

			/* Succeeded: had it. */
			*merge = PH_MERGE_KNOWN;
			return 0;
		}
	}

	/* The conversation of its number. */
	conversation = ph_store_conversation(message->peer, message->name, 1);
	if (conversation < 0)
		return ENOMEM;
	*contact = conversation;

	/* Step 3: one without a key is kept unless one like it is there (with its key or not). */
	if (partial) {
		matched = store_candidate(conversation, message, 1, &found);
		if (matched) {
			*item = found;
			*merge = PH_MERGE_KNOWN;
			return 0;
		}

		/* A new file, marked partial. */
		error = store_new_message(conversation, message, 1, &placed);
		if (error != 0)
			return error;
		*item = placed;
		*merge = PH_MERGE_NEW;
		return 0;
	}

	/* Step 2: one of the store's own without its key takes it. */
	matched = store_candidate(conversation, message, 0, &found);
	if (matched) {
		error = store_overlay(conversation, found, message, &placed);
		if (error != 0)
			return error;
		*item = placed;
		*merge = PH_MERGE_OVERLAID;
		return 0;
	}

	/* A new file. */
	error = store_new_message(conversation, message, 0, &placed);
	if (error != 0)
		return error;

	/* Succeeded: kept. */
	*item = placed;
	*merge = PH_MERGE_NEW;
	return 0;
}

/*
 * Finds an item by its number for this run: 0 with its contact and index,
 * or ENOENT.
 */
int
ph_store_find_serial(
	unsigned long serial,
	long *contact,
	size_t *item)
{
	size_t index;
	size_t at;

	/* Each item of each contact. */
	for (index = 0U; index < store_contact_count; index++) {
		for (at = 0U; at < store_contacts[index].item_count; at++) {
			if (store_contacts[index].items[at].serial != serial)
				continue;
			*contact = (long)index;
			*item = at;
			return 0;
		}
	}

	/* None. */
	return ENOENT;
}

/*
 * Reads how far a paired phone's messages were brought in (ws197-p004
 * section 6.2): 0 with the times, ENOENT when nothing was, or EINVAL for
 * an address that is not one.
 */
int
ph_store_sync_load(
	const char *address,
	int64_t *since,
	int64_t *deep_at)
{
	char path[STORE_PATH_MAX];
	char line[STORE_LINE_MAX];
	long long value;
	char *read_line;
	FILE *file;
	int fields;
	int error;

	/* The file of the phone. */
	error = store_sync_path(address, path, sizeof(path));
	if (error != 0)
		return error;
	file = fopen(path, "r");
	if (file == NULL)
		return ENOENT;

	/* Each line it knows. */
	*since = 0;
	*deep_at = 0;
	for (;;) {
		read_line = fgets(line, (int)sizeof(line), file);
		if (read_line == NULL)
			break;
		fields = sscanf(line, "messages_since %lld", &value);
		if (fields == 1) {
			*since = (int64_t)value;
			continue;
		}

		/* The last deep synchronisation. */
		fields = sscanf(line, "deep_at %lld", &value);
		if (fields == 1)
			*deep_at = (int64_t)value;
	}

	/* Succeeded: read. */
	fclose(file);
	return 0;
}

/*
 * Writes how far a paired phone's messages were brought in.  Returns 0,
 * EINVAL for an address that is not one, or an errno value.
 */
int
ph_store_sync_save(
	const char *address,
	int64_t since,
	int64_t deep_at)
{
	char path[STORE_PATH_MAX];
	char text[128];
	int length;
	int error;

	/* The file of the phone and its lines. */
	error = store_sync_path(address, path, sizeof(path));
	if (error != 0)
		return error;
	length = snprintf(text, sizeof(text), "messages_since %lld\ndeep_at %lld\n", (long long)since, (long long)deep_at);
	if (length < 0 || (size_t)length >= sizeof(text))
		return E2BIG;

	/* Written. */
	error = store_write_file(path, text, (size_t)length);
	if (error != 0)
		return error;

	/* Succeeded: kept. */
	return 0;
}

/* Reads every contact's file. */
static int
store_load_contacts(void)
{
	char path[STORE_PATH_MAX];
	char id[256];
	struct dirent *entry;
	size_t length;
	DIR *folder;
	int named;
	int error;

	/* The folder. */
	(void)snprintf(path, sizeof(path), "%s/contacts", store_root);
	folder = opendir(path);
	if (folder == NULL)
		return errno;

	/* Each .vcf file. */
	error = 0;
	for (;;) {
		entry = readdir(folder);
		if (entry == NULL)
			break;

		/* Another file, or a name too long for an ID. */
		named = store_is_named(entry->d_name, ".vcf");
		length = strlen(entry->d_name);
		if (!named || length - 4U >= sizeof(id))
			continue;

		/* The contact of its ID. */
		memcpy(id, entry->d_name, length - 4U);
		id[length - 4U] = '\0';
		error = store_load_contact(id);
		if (error != 0)
			break;
	}

	/* The folder is read. */
	closedir(folder);

	/* A failure to keep one. */
	if (error != 0)
		return error;

	/* Succeeded: every contact is read. */
	return 0;
}

/* Reads one contact's file and its items. */
static int
store_load_contact(
	const char *id)
{
	char path[STORE_PATH_MAX];
	char line[STORE_LINE_MAX];
	char name[STORE_LINE_MAX];
	char number[STORE_LINE_MAX];
	char *colon;
	char *end;
	FILE *file;
	int same;
	int error;

	/* The file. */
	(void)snprintf(path, sizeof(path), "%s/contacts/%s.vcf", store_root, id);
	file = fopen(path, "r");
	if (file == NULL)
		return 0;

	/* Its FN and TEL lines (a parameter after TEL, "TEL;TYPE=cell:", is allowed). */
	name[0] = '\0';
	number[0] = '\0';
	for (;;) {
		end = fgets(line, (int)sizeof(line), file);
		if (end == NULL)
			break;

		/* The line without its end, and its value after the colon. */
		line[strcspn(line, "\r\n")] = '\0';
		colon = strchr(line, ':');
		if (colon == NULL)
			continue;
		*colon = '\0';

		/* The name. */
		same = strcmp(line, "FN");
		if (same == 0) {
			(void)snprintf(name, sizeof(name), "%s", colon + 1);
			continue;
		}

		/* The number, with or without parameters. */
		same = strncmp(line, "TEL", 3U);
		if (same == 0)
			(void)snprintf(number, sizeof(number), "%s", colon + 1);
	}

	/* The file is read. */
	fclose(file);

	/* A contact without a number is not one. */
	if (number[0] == '\0')
		return 0;
	if (name[0] == '\0')
		(void)snprintf(name, sizeof(name), "%s", number);

	/* Kept, then its items. */
	error = store_append_contact(id, name, number);
	if (error != 0)
		return error;
	error = store_load_items((long)store_contact_count - 1L, id);
	if (error != 0)
		return error;

	/* Succeeded: the contact is read. */
	return 0;
}

/*
 * Reads the numbers' conversations (ws197-p004b): each folder of messages
 * that is no contact's, named for a number's key, is read as the contact's
 * of that number, or else as a conversation of its own.
 */
static int
store_load_conversations(void)
{
	char path[STORE_PATH_MAX];
	char key[PH_NUMBER_KEY_MAX];
	struct dirent *entry;
	struct ph_contact *kept;
	size_t index;
	long contact;
	char *name;
	DIR *folder;
	int decoded;
	int failed;
	int same;
	int error;

	/* The folder of the messages. */
	(void)snprintf(path, sizeof(path), "%s/messages", store_root);
	folder = opendir(path);
	if (folder == NULL)
		return 0;

	/* Each folder named for a number's key, not a contact's own. */
	error = 0;
	for (;;) {
		entry = readdir(folder);
		if (entry == NULL)
			break;
		decoded = store_folder_key(entry->d_name, key, sizeof(key));
		if (decoded != 0)
			continue;

		/* A contact's own folder was read with the contact. */
		same = 1;
		for (index = 0; index < store_contact_count; index++) {
			same = strcmp(store_contacts[index].id, entry->d_name);
			if (same == 0)
				break;
		}

		/* Read already. */
		if (same == 0)
			continue;

		/* The contact of that number, or a conversation of its own named by the number. */
		contact = ph_store_conversation(key, NULL, 0);
		if (contact < 0) {
			error = store_append_contact(entry->d_name, key, key);
			if (error != 0)
				break;
			contact = (long)store_contact_count - 1L;
			store_contacts[contact].conversation = 1;
		}

		/* Its items. */
		error = store_load_items(contact, entry->d_name);
		if (error != 0)
			break;

		/* A conversation of its own is named as the phone named the other side last. */
		kept = &store_contacts[contact];
		if (!kept->conversation)
			continue;
		for (index = kept->item_count; index > 0U; index--) {
			if (kept->items[index - 1U].name == NULL)
				continue;

			/* The name (the key stays without memory). */
			failed = 0;
			name = store_copy(kept->items[index - 1U].name, &failed);
			if (failed)
				break;
			free(kept->name);
			kept->name = name;
			store_initials(kept->items[index - 1U].name, kept->initials, sizeof(kept->initials));
			kept->color = store_color(kept->items[index - 1U].name);
			break;
		}
	}

	/* The folder is read. */
	closedir(folder);
	if (error != 0)
		return error;

	/* Succeeded: every conversation is read. */
	return 0;
}

/* Reads a contact's items of a folder (its own, or a number's read as its), oldest first. */
static int
store_load_items(
	long contact,
	const char *folder_name)
{
	char path[STORE_PATH_MAX];
	char item_path[STORE_PATH_MAX + 256U + 2U];
	struct dirent *entry;
	DIR *folder;
	int named;
	int error;

	/* The folder (a contact without one has no items). */
	(void)snprintf(path, sizeof(path), "%s/messages/%s", store_root, folder_name);
	folder = opendir(path);
	if (folder == NULL)
		return 0;

	/* Each .txt file. */
	error = 0;
	for (;;) {
		entry = readdir(folder);
		if (entry == NULL)
			break;
		named = store_is_named(entry->d_name, ".txt");
		if (!named)
			continue;

		/* The item of the file. */
		(void)snprintf(item_path, sizeof(item_path), "%s/%s", path, entry->d_name);
		error = store_load_item(contact, item_path);
		if (error != 0)
			break;
	}

	/* The folder is read; a failure to keep an item ends the reading. */
	closedir(folder);
	if (error != 0)
		return error;

	/* Succeeded: the timeline is read, oldest first. */
	store_sort_items(contact);
	return 0;
}

/* Sorts a contact's timeline, oldest first. */
static void
store_sort_items(
	long contact)
{
	struct ph_contact *kept;
	struct ph_item moved;
	size_t i;
	size_t at;

	/* Each moved back past the newer ones before it. */
	kept = &store_contacts[contact];
	for (i = 1; i < kept->item_count; i++) {
		moved = kept->items[i];
		at = i;
		while (at > 0U && kept->items[at - 1U].date > moved.date) {
			kept->items[at] = kept->items[at - 1U];
			at--;
		}

		/* Its place. */
		kept->items[at] = moved;
	}
}

/* Reads one item's file. */
static int
store_load_item(
	long contact,
	const char *path)
{
	struct ph_item item;
	char *extra;
	char *text;
	char *line;
	char *next;
	char *colon;
	char *body;
	size_t extra_length;
	size_t length;
	FILE *file;
	int known;
	int error;

	/* The file's bytes. */
	file = fopen(path, "r");
	if (file == NULL)
		return 0;
	text = malloc(STORE_ITEM_MAX);
	if (text == NULL) {
		fclose(file);
		return ENOMEM;
	}

	/* Up to the most an item keeps. */
	length = fread(text, 1U, STORE_ITEM_MAX - 1U, file);
	fclose(file);
	text[length] = '\0';

	/* The header's lines up to the empty one (those not known kept as they were). */
	memset(&item, 0, sizeof(item));
	item.path = (char *)path;
	body = NULL;
	extra = NULL;
	extra_length = 0U;
	error = 0;
	for (line = text; line != NULL && *line != '\0'; line = next) {
		next = strchr(line, '\n');
		if (next != NULL) {
			*next = '\0';
			next++;
		}

		/* Without a CR. */
		line[strcspn(line, "\r")] = '\0';

		/* The empty line: the words after it. */
		if (line[0] == '\0') {
			body = next;
			break;
		}

		/* A key and its value. */
		colon = strchr(line, ':');
		if (colon == NULL)
			continue;
		*colon = '\0';
		colon++;
		while (*colon == ' ')
			colon++;
		known = store_item_field(&item, line, colon);
		if (known)
			continue;

		/* A line of a later program's. */
		error = store_extra_add(&extra, &extra_length, line, colon);
		if (error != 0)
			break;
	}

	/* No memory for the lines not known. */
	if (error != 0) {
		free(extra);
		free(text);
		return error;
	}

	/* The words, without the line end at their end. */
	if (body != NULL) {
		length = strlen(body);
		while (length > 0U && (body[length - 1U] == '\n' || body[length - 1U] == '\r'))
			length--;
		body[length] = '\0';
		if (length != 0U)
			item.text = body;
	}

	/* Kept. */
	item.extra = extra;
	error = store_append_item(contact, &item);
	free(extra);
	free(text);
	if (error != 0)
		return error;

	/* Succeeded: the item is read. */
	return 0;
}

/* Adds a header's line not known ("key: value") to those kept.  Returns 0 or ENOMEM. */
static int
store_extra_add(
	char **extra,
	size_t *length,
	const char *key,
	const char *value)
{
	char *grown;
	size_t added;

	/* Room for the line and the NUL. */
	added = strlen(key) + 2U + strlen(value) + 1U;
	grown = realloc(*extra, *length + added + 1U);
	if (grown == NULL)
		return ENOMEM;

	/* The line after the others. */
	(void)snprintf(grown + *length, added + 1U, "%s: %s\n", key, value);
	*extra = grown;
	*length += added;
	return 0;
}

/* Appends a contact (its strings copied, its initials and color made). */
static int
store_append_contact(
	const char *id,
	const char *name,
	const char *number)
{
	struct ph_contact *grown;
	struct ph_contact *kept;
	size_t capacity;
	int failed;

	/* Room for one more. */
	if (store_contact_count == STORE_CONTACTS_MAX)
		return ENOSPC;
	if (store_contact_count == store_contact_capacity) {
		capacity = store_contact_capacity * 2U;
		if (capacity == 0U)
			capacity = 16U;
		grown = realloc(store_contacts, capacity * sizeof(grown[0]));
		if (grown == NULL)
			return ENOMEM;
		store_contacts = grown;
		store_contact_capacity = capacity;
	}

	/* The contact. */
	kept = &store_contacts[store_contact_count];
	memset(kept, 0, sizeof(*kept));
	failed = 0;
	kept->id = store_copy(id, &failed);
	kept->name = store_copy(name, &failed);
	kept->number = store_copy(number, &failed);
	if (failed) {
		free(kept->id);
		free(kept->name);
		free(kept->number);
		return ENOMEM;
	}

	/* The picture standing for the person. */
	store_initials(name, kept->initials, sizeof(kept->initials));
	kept->color = store_color(name);

	/* Succeeded: one more contact. */
	store_contact_count++;
	return 0;
}

/* Appends an item to a contact's timeline (its strings copied, its words made). */
static int
store_append_item(
	long contact,
	const struct ph_item *item)
{
	struct ph_contact *kept;
	struct ph_item *grown;
	struct ph_item *added;
	size_t capacity;
	int failed;

	/* Room for one more. */
	kept = &store_contacts[contact];
	if (kept->item_count == kept->item_capacity) {
		capacity = kept->item_capacity * 2U;
		if (capacity == 0U)
			capacity = 16U;
		grown = realloc(kept->items, capacity * sizeof(grown[0]));
		if (grown == NULL)
			return ENOMEM;
		kept->items = grown;
		kept->item_capacity = capacity;
	}

	/* The item, its strings of its own. */
	added = &kept->items[kept->item_count];
	*added = *item;
	added->day = NULL;
	added->time = NULL;
	failed = 0;
	added->text = store_copy(item->text, &failed);
	added->detail = store_copy(item->detail, &failed);
	added->path = store_copy(item->path, &failed);
	added->source = store_copy(item->source, &failed);
	added->name = store_copy(item->name, &failed);
	added->extra = store_copy(item->extra, &failed);
	if (failed) {
		store_free_item(added);
		return ENOMEM;
	}

	/* Its number for this run, and the day and time as words. */
	store_item_serial++;
	added->serial = store_item_serial;
	store_words(added, time(NULL));

	/* Counted; an unread one is counted for the list's dot. */
	kept->item_count++;
	if (added->state == PH_STATE_UNREAD)
		kept->unread++;
	return 0;
}

/* Writes an item's file. */
static int
store_write_item(
	const struct ph_item *item,
	const char *path)
{
	char clean[STORE_LINE_MAX];
	const char *words;
	size_t used;
	char *text;
	int error;

	/* Room for the header and the words. */
	text = malloc(STORE_ITEM_MAX);
	if (text == NULL)
		return ENOMEM;

	/* The header: the fields every item has. */
	used = 0U;
	error = store_put(text, &used, "Kind: %s\nChannel: %s\nDirection: %s\nDate: %lld\nState: %s\n", store_kinds[item->kind], store_channels[item->channel],
	    store_direction(item->outgoing), (long long)item->date, store_states[item->state]);

	/* The detail, and the paired phone's fields (ws197-p004b). */
	if (error == 0 && item->detail != NULL)
		error = store_put(text, &used, "Detail: %s\n", item->detail);
	if (error == 0 && item->source != NULL)
		error = store_put(text, &used, "Source: %s\n", item->source);
	if (error == 0 && item->partial)
		error = store_put(text, &used, "Partial: yes\n");
	if (error == 0 && item->truncated)
		error = store_put(text, &used, "Truncated: yes\n");
	if (error == 0 && item->name != NULL) {
		store_clean_line(item->name, clean, sizeof(clean));
		error = store_put(text, &used, "Name: %s\n", clean);
	}

	/* The lines a later program wrote, as they were. */
	if (error == 0 && item->extra != NULL)
		error = store_put(text, &used, "%s", item->extra);

	/* The empty line and the words. */
	words = "";
	if (item->text != NULL)
		words = item->text;
	if (error == 0)
		error = store_put(text, &used, "\n%s\n", words);

	/* An item larger than a file keeps. */
	if (error != 0) {
		free(text);
		return error;
	}

	/* Written. */
	error = store_write_file(path, text, used);
	free(text);
	if (error != 0)
		return error;

	/* Succeeded: the file holds the item. */
	return 0;
}

/* Adds formatted text to an item's file being made.  Returns 0, or E2BIG when it does not fit. */
static int
store_put(
	char *text,
	size_t *used,
	const char *format,
	...)
{
	va_list arguments;
	int length;

	/* The text after what is there. */
	va_start(arguments, format);
	length = vsnprintf(text + *used, STORE_ITEM_MAX - *used, format, arguments);
	va_end(arguments);

	/* Larger than the room left. */
	if (length < 0 || (size_t)length >= STORE_ITEM_MAX - *used)
		return E2BIG;

	/* Succeeded: added. */
	*used += (size_t)length;
	return 0;
}

/* Writes a file beside its place and renames it over it. */
static int
store_write_file(
	const char *path,
	const char *text,
	size_t length)
{
	char fresh[STORE_PATH_MAX + 8U];
	size_t written;
	FILE *file;
	int status;

	/* The new file. */
	(void)snprintf(fresh, sizeof(fresh), "%s.new", path);
	file = fopen(fresh, "w");
	if (file == NULL)
		return errno;
	written = fwrite(text, 1U, length, file);
	status = fclose(file);
	if (written != length || status != 0)
		return EIO;

	/* In place. */
	status = rename(fresh, path);
	if (status != 0)
		return errno;

	/* Succeeded: the file is written. */
	return 0;
}

/*
 * Sets an item's field from a header line's key and value (a value not
 * known is skipped).  Returns 1 for a key this program knows, 0 for one it
 * keeps as it was.
 */
static int
store_item_field(
	struct ph_item *item,
	const char *key,
	char *value)
{
	int found;
	int same;

	/* Kind. */
	same = strcmp(key, "Kind");
	if (same == 0) {
		found = store_word(store_kinds, 4U, value);
		if (found >= 0)
			item->kind = (enum ph_kind)found;
		return 1;
	}

	/* Channel. */
	same = strcmp(key, "Channel");
	if (same == 0) {
		found = store_word(store_channels, 5U, value);
		if (found >= 0)
			item->channel = (enum ph_channel)found;
		return 1;
	}

	/* Direction. */
	same = strcmp(key, "Direction");
	if (same == 0) {
		same = strcmp(value, "out");
		item->outgoing = 0;
		if (same == 0)
			item->outgoing = 1;
		return 1;
	}

	/* Date. */
	same = strcmp(key, "Date");
	if (same == 0) {
		item->date = (time_t)strtoll(value, NULL, 10);
		return 1;
	}

	/* State. */
	same = strcmp(key, "State");
	if (same == 0) {
		found = store_word(store_states, PH_STATES, value);
		if (found >= 0)
			item->state = (enum ph_state)found;
		return 1;
	}

	/* Detail (the line's own text, copied when the item is kept). */
	same = strcmp(key, "Detail");
	if (same == 0) {
		item->detail = value;
		return 1;
	}

	/* The paired phone's message: where it came from (ws197-p004b). */
	same = strcmp(key, "Source");
	if (same == 0) {
		item->source = value;
		return 1;
	}

	/* Its time was not known. */
	same = strcmp(key, "Partial");
	if (same == 0) {
		same = strcmp(value, "yes");
		item->partial = 0;
		if (same == 0)
			item->partial = 1;
		return 1;
	}

	/* Its words were cut. */
	same = strcmp(key, "Truncated");
	if (same == 0) {
		same = strcmp(value, "yes");
		item->truncated = 0;
		if (same == 0)
			item->truncated = 1;
		return 1;
	}

	/* The other side's name as the phone gave it. */
	same = strcmp(key, "Name");
	if (same == 0) {
		item->name = value;
		return 1;
	}

	/* A key of a later program's. */
	return 0;
}

/* Finds a word in a table; its index, or -1. */
static int
store_word(
	const char *const *words,
	size_t count,
	const char *word)
{
	size_t index;
	int same;

	/* Each word. */
	for (index = 0; index < count; index++) {
		same = strcmp(words[index], word);
		if (same == 0)
			return (int)index;
	}

	/* Not known. */
	return -1;
}

/* Makes an item's day and time as words ("Today", "Yesterday", "Sat, 3 Oct"; "09:41") and an outgoing message's detail from its state. */
static void
store_words(
	struct ph_item *item,
	time_t now)
{
	struct tm when;
	struct tm today;
	char day[32];
	char clock[16];
	const char *state;
	char *copied;
	int failed;

	/* The date and today in the local time. */
	(void)localtime_r(&item->date, &when);
	(void)localtime_r(&now, &today);

	/* The day. */
	if (when.tm_year == today.tm_year && when.tm_yday == today.tm_yday)
		(void)snprintf(day, sizeof(day), "Today");
	else if (when.tm_year == today.tm_year && today.tm_yday - when.tm_yday == 1)
		(void)snprintf(day, sizeof(day), "Yesterday");
	else
		(void)snprintf(day, sizeof(day), "%s, %d %s", store_days[when.tm_wday], when.tm_mday, store_months[when.tm_mon]);
	(void)snprintf(clock, sizeof(clock), "%02d:%02d", when.tm_hour, when.tm_min);

	/* Kept (an old pair is freed). */
	failed = 0;
	free(item->day);
	free(item->time);
	item->day = store_copy(day, &failed);
	item->time = store_copy(clock, &failed);

	/* An outgoing message's detail is its state. */
	if (item->kind != PH_TEXT || !item->outgoing)
		return;
	state = NULL;
	if (item->state == PH_STATE_SENDING || item->state == PH_STATE_UNKNOWN)
		state = "Sending";
	else if (item->state == PH_STATE_SENT)
		state = "Sent";
	else if (item->state == PH_STATE_DELIVERED)
		state = "Delivered";
	else if (item->state == PH_STATE_FAILED)
		state = "Not delivered";
	if (state == NULL)
		return;
	copied = store_copy(state, &failed);
	if (copied == NULL)
		return;
	free(item->detail);
	item->detail = copied;
}

/* Makes the initials of a name: the first character of its first and last words (one for one word). */
static void
store_initials(
	const char *name,
	char *initials,
	size_t size)
{
	const char *last;
	size_t first_length;
	size_t last_length;

	/* The first character (all of its UTF-8 bytes). */
	first_length = 1;
	while (name[first_length] != '\0' && ((unsigned char)name[first_length] & 0xc0U) == 0x80U)
		first_length++;

	/* The last word's first character, when there is a second word. */
	last = strrchr(name, ' ');
	last_length = 0;
	if (last != NULL && last[1] != '\0') {
		last++;
		last_length = 1;
		while (last[last_length] != '\0' && ((unsigned char)last[last_length] & 0xc0U) == 0x80U)
			last_length++;
	}

	/* Both, when they fit. */
	initials[0] = '\0';
	if (first_length + last_length >= size)
		return;
	memcpy(initials, name, first_length);
	if (last_length != 0U)
		memcpy(initials + first_length, last, last_length);
	initials[first_length + last_length] = '\0';
}

/* Chooses a contact's color by its name. */
static kl_color
store_color(
	const char *name)
{
	unsigned long hash;
	size_t index;

	/* A small hash of the name. */
	hash = 5381UL;
	for (index = 0; name[index] != '\0'; index++)
		hash = hash * 33UL + (unsigned char)name[index];

	/* One of the colors. */
	return store_colors[hash % 8UL];
}

/* Sorts the contacts by their latest item, the latest first (those without items last). */
static void
store_sort_contacts(void)
{
	struct ph_contact moved;
	time_t moved_latest;
	time_t before_latest;
	size_t i;
	size_t at;

	/* Each moved up past the older ones before it. */
	for (i = 1; i < store_contact_count; i++) {
		moved = store_contacts[i];
		at = i;
		moved_latest = store_latest(&moved);
		while (at > 0U) {
			/* A later one before it stays before it. */
			before_latest = store_latest(&store_contacts[at - 1U]);
			if (before_latest >= moved_latest)
				break;
			store_contacts[at] = store_contacts[at - 1U];
			at--;
		}

		/* Its place. */
		store_contacts[at] = moved;
	}
}

/* Reports the date of a contact's latest item (0 for none). */
static time_t
store_latest(
	const struct ph_contact *contact)
{
	/* No item. */
	if (contact->item_count == 0U)
		return 0;

	/* The last of the timeline. */
	return contact->items[contact->item_count - 1U].date;
}

/* Frees an item's strings. */
static void
store_free_item(
	struct ph_item *item)
{
	/* Each string (free takes NULL). */
	free(item->day);
	free(item->time);
	free(item->text);
	free(item->detail);
	free(item->path);
	free(item->source);
	free(item->name);
	free(item->extra);
}

/* Copies a string (NULL stays NULL); a failure is noted. */
static char *
store_copy(
	const char *text,
	int *failed)
{
	char *copied;
	size_t length;

	/* Nothing to copy. */
	if (text == NULL)
		return NULL;

	/* The copy. */
	length = strlen(text);
	copied = malloc(length + 1U);
	if (copied == NULL) {
		*failed = 1;
		return NULL;
	}

	/* Its bytes and NUL. */
	memcpy(copied, text, length + 1U);
	return copied;
}

/* Reports the word of an item's direction in its file and name. */
static const char *
store_direction(
	int outgoing)
{
	/* Out. */
	if (outgoing)
		return "out";

	/* In. */
	return "in";
}

/* Tells whether a file's name ends with a suffix (and has something before it). */
static int
store_is_named(
	const char *name,
	const char *suffix)
{
	size_t length;
	size_t suffix_length;
	int same;

	/* Longer than the suffix. */
	length = strlen(name);
	suffix_length = strlen(suffix);
	if (length <= suffix_length)
		return 0;

	/* Ending with it. */
	same = strcmp(name + length - suffix_length, suffix);
	if (same != 0)
		return 0;

	/* Named so. */
	return 1;
}

/*
 * Names the folder of a number's key: "n" and the digits of "+<digits>" (at
 * least six) or of a short number, else "a" and the key's bytes in
 * hexadecimal.  Returns 0, or EINVAL without room.
 */
static int
store_key_folder(
	const char *key,
	char *folder,
	size_t size)
{
	size_t length;
	size_t span;
	size_t index;
	int written;

	/* "+" and six digits or more. */
	length = strlen(key);
	if (key[0] == '+') {
		span = strspn(key + 1, "0123456789");
		if (span == length - 1U && span >= 6U) {
			written = snprintf(folder, size, "n%s", key + 1);
			if (written < 0 || (size_t)written >= size)
				return EINVAL;
			return 0;
		}
	}

	/* A short number's digits. */
	span = strspn(key, "0123456789");
	if (length > 0U && span == length && length < 6U) {
		written = snprintf(folder, size, "n%s", key);
		if (written < 0 || (size_t)written >= size)
			return EINVAL;
		return 0;
	}

	/* Words: their bytes in hexadecimal. */
	if (length == 0U || 1U + length * 2U >= size)
		return EINVAL;
	folder[0] = 'a';
	for (index = 0U; index < length; index++)
		(void)snprintf(folder + 1U + index * 2U, 3U, "%02x", (unsigned char)key[index]);

	/* Succeeded: the folder's name. */
	return 0;
}

/*
 * Reads the number's key a folder is named for (store_key_folder's
 * inverse).  Returns 0, or EINVAL for a folder of another name.
 */
static int
store_folder_key(
	const char *folder,
	char *key,
	size_t size)
{
	unsigned value;
	size_t length;
	size_t span;
	size_t index;
	int fields;
	int written;

	/* The digits of a number. */
	length = strlen(folder);
	if (folder[0] == 'n') {
		span = strspn(folder + 1, "0123456789");
		if (length < 2U || span != length - 1U)
			return EINVAL;
		if (span < 6U) {
			/* A short number. */
			written = snprintf(key, size, "%s", folder + 1);
		} else {
			/* A number with its country code. */
			written = snprintf(key, size, "+%s", folder + 1);
		}

		/* No room. */
		if (written < 0 || (size_t)written >= size)
			return EINVAL;
		return 0;
	}

	/* The bytes of words in hexadecimal. */
	if (folder[0] != 'a' || length < 3U || (length - 1U) % 2U != 0U)
		return EINVAL;
	span = strspn(folder + 1, "0123456789abcdef");
	if (span != length - 1U || (length - 1U) / 2U >= size)
		return EINVAL;
	for (index = 0U; index < (length - 1U) / 2U; index++) {
		fields = sscanf(folder + 1U + index * 2U, "%2x", &value);
		if (fields != 1 || value == 0U)
			return EINVAL;
		key[index] = (char)value;
	}

	/* Its end. */
	key[(length - 1U) / 2U] = '\0';

	/* Succeeded: the key. */
	return 0;
}

/* Tells whether a message's key may name a file: 1 to 16 hexadecimal digits. */
static int
store_key_valid(
	const char *key)
{
	size_t length;
	size_t span;

	/* Hexadecimal digits alone, at most sixteen. */
	length = strlen(key);
	span = strspn(key, "0123456789abcdefABCDEF");
	if (length == 0U || length >= STORE_KEY_MAX || span != length)
		return 0;

	/* Succeeded: a key. */
	return 1;
}

/*
 * Finds the item of a message's key: its source ends with ":map:<key>",
 * or its file is s<key>.txt (a program that dropped the source).  Returns
 * 1 with its contact and index, or 0.
 */
static int
store_find_key(
	const char *key,
	long *contact,
	size_t *item)
{
	char suffix[STORE_KEY_MAX + 8U];
	char file_name[STORE_KEY_MAX + 8U];
	const struct ph_item *kept;
	const char *base;
	size_t length;
	size_t suffix_length;
	size_t index;
	size_t at;
	int same;

	/* The source's end and the file's name of the key. */
	(void)snprintf(suffix, sizeof(suffix), ":map:%s", key);
	(void)snprintf(file_name, sizeof(file_name), "s%s.txt", key);
	suffix_length = strlen(suffix);

	/* Each item of each contact. */
	for (index = 0U; index < store_contact_count; index++) {
		for (at = 0U; at < store_contacts[index].item_count; at++) {
			kept = &store_contacts[index].items[at];

			/* By its source. */
			if (kept->source != NULL) {
				length = strlen(kept->source);
				if (length >= suffix_length) {
					same = strcmp(kept->source + length - suffix_length, suffix);
					if (same == 0) {
						*contact = (long)index;
						*item = at;
						return 1;
					}
				}
			}

			/* By its file's name. */
			if (kept->path == NULL)
				continue;
			base = strrchr(kept->path, '/');
			if (base == NULL)
				base = kept->path;
			else
				base++;
			same = strcmp(base, file_name);
			if (same != 0)
				continue;
			*contact = (long)index;
			*item = at;
			return 1;
		}
	}

	/* None. */
	return 0;
}

/*
 * Finds the item of a conversation a message of the phone is (section
 * 6.3's candidates): a message the same way, of the same words, within ten
 * minutes, without a source unless with_source is 1, the nearest in time.
 * Returns 1 with its index, or 0.
 */
static int
store_candidate(
	long contact,
	const struct ph_phone_message *message,
	int with_source,
	size_t *item)
{
	const struct ph_item *kept;
	long long difference;
	long long best_difference;
	size_t index;
	int found;
	int same;

	/* Each message of the conversation. */
	found = 0;
	best_difference = 0;
	for (index = 0U; index < store_contacts[contact].item_count; index++) {
		kept = &store_contacts[contact].items[index];
		if (kept->kind != PH_TEXT || kept->outgoing != message->outgoing)
			continue;
		if (kept->source != NULL && !with_source)
			continue;

		/* Within ten minutes. */
		difference = (long long)kept->date - (long long)message->date;
		if (difference < 0)
			difference = -difference;
		if (difference > STORE_MATCH_SECONDS)
			continue;

		/* The same words. */
		same = store_same_words(kept->text, message->text);
		if (!same)
			continue;

		/* The nearest in time. */
		if (found && difference >= best_difference)
			continue;
		found = 1;
		best_difference = difference;
		*item = index;
	}

	/* Found or not. */
	return found;
}

/*
 * Tells whether the words kept are those that came: byte for byte, a CR
 * before an LF aside, and the line ends at their ends aside (the file
 * keeps none).  NULL is no words.
 */
static int
store_same_words(
	const char *kept,
	const char *came)
{
	size_t kept_length;
	size_t came_length;
	size_t i;
	size_t j;

	/* Their lengths without the line ends at their ends. */
	if (kept == NULL)
		kept = "";
	kept_length = strlen(kept);
	while (kept_length > 0U && (kept[kept_length - 1U] == '\n' || kept[kept_length - 1U] == '\r'))
		kept_length--;
	came_length = strlen(came);
	while (came_length > 0U && (came[came_length - 1U] == '\n' || came[came_length - 1U] == '\r'))
		came_length--;

	/* Each byte, a CR before an LF skipped on either side. */
	i = 0U;
	j = 0U;
	while (i < kept_length && j < came_length) {
		if (kept[i] == '\r' && i + 1U < kept_length && kept[i + 1U] == '\n') {
			i++;
			continue;
		}

		/* A CR before an LF that came. */
		if (came[j] == '\r' && j + 1U < came_length && came[j + 1U] == '\n') {
			j++;
			continue;
		}

		/* A byte that differs. */
		if (kept[i] != came[j])
			return 0;
		i++;
		j++;
	}

	/* Both at their ends. */
	if (i != kept_length || j != came_length)
		return 0;

	/* Succeeded: the same. */
	return 1;
}

/*
 * Gives one of the store's messages without a source the phone's key
 * (step 2): its source, the phone's time, no longer partial, one's own
 * sent (delivered stays), its file renamed s<key>.txt.  *placed is where
 * it is in the timeline after.  Returns 0 or an errno value.
 */
static int
store_overlay(
	long contact,
	size_t item,
	const struct ph_phone_message *message,
	size_t *placed)
{
	char source[STORE_LINE_MAX];
	char path[STORE_PATH_MAX];
	struct ph_item *kept;
	const char *slash;
	char *new_source;
	char *new_path;
	char *new_name;
	int failed;
	int status;
	int error;

	/* The source, and the file's new name beside the old. */
	kept = &store_contacts[contact].items[item];
	(void)snprintf(source, sizeof(source), "bt:%s:map:%s", message->address, message->key);
	slash = strrchr(kept->path, '/');
	if (slash == NULL)
		return EINVAL;
	(void)snprintf(path, sizeof(path), "%.*s/s%s.txt", (int)(slash - kept->path), kept->path, message->key);
	failed = 0;
	new_source = store_copy(source, &failed);
	new_path = store_copy(path, &failed);
	new_name = NULL;
	if (kept->name == NULL && message->name != NULL && message->name[0] != '\0')
		new_name = store_copy(message->name, &failed);
	if (failed) {
		free(new_source);
		free(new_path);
		free(new_name);
		return ENOMEM;
	}

	/* The item takes the phone's key and time; one's own sent text is sent (delivered stays). */
	free(kept->source);
	kept->source = new_source;
	if (new_name != NULL)
		kept->name = new_name;
	kept->date = message->date;
	kept->partial = 0;
	if (kept->outgoing && kept->state != PH_STATE_DELIVERED)
		kept->state = PH_STATE_SENT;

	/* Written, then renamed to its key's name. */
	error = store_write_item(kept, kept->path);
	if (error != 0) {
		free(new_path);
		return error;
	}

	/* The new name (the old one kept when it cannot be renamed). */
	status = rename(kept->path, path);
	if (status == 0) {
		free(kept->path);
		kept->path = new_path;
	} else {
		free(new_path);
	}

	/* Succeeded: its words and its place by its time. */
	store_words(kept, time(NULL));
	*placed = store_place_item(contact, item);
	return 0;
}

/*
 * Keeps a new message of the phone in a conversation: s<key>.txt with its
 * source, or (partial) a file of its time and this run's number, marked
 * partial.  *placed is where it is in the timeline.  Returns 0 or an errno
 * value.
 */
static int
store_new_message(
	long contact,
	const struct ph_phone_message *message,
	int partial,
	size_t *placed)
{
	struct ph_item made;
	char source[STORE_LINE_MAX];
	char path[STORE_PATH_MAX];
	int error;

	/* Its file's name and source. */
	memset(&made, 0, sizeof(made));
	if (partial) {
		store_serial++;
		(void)snprintf(path, sizeof(path), "%s/messages/%s/%lld-%lu-%s.txt", store_root, store_contacts[contact].id, (long long)message->date, store_serial,
		    store_direction(message->outgoing));
		made.partial = 1;
	} else {
		(void)snprintf(path, sizeof(path), "%s/messages/%s/s%s.txt", store_root, store_contacts[contact].id, message->key);
		(void)snprintf(source, sizeof(source), "bt:%s:map:%s", message->address, message->key);
		made.source = source;
	}

	/* The message: received unread unless the phone read it, one's own sent. */
	made.kind = PH_TEXT;
	made.channel = PH_SMS;
	made.outgoing = message->outgoing;
	made.date = message->date;
	made.state = PH_STATE_UNREAD;
	if (message->read)
		made.state = PH_STATE_READ;
	if (message->outgoing)
		made.state = PH_STATE_SENT;
	made.text = (char *)message->text;
	made.truncated = message->truncated;
	if (message->name != NULL && message->name[0] != '\0')
		made.name = (char *)message->name;
	made.path = path;

	/* Written. */
	error = store_write_item(&made, path);
	if (error != 0)
		return error;

	/* Kept, in its place by its time. */
	error = store_append_item(contact, &made);
	if (error != 0)
		return error;

	/* Succeeded: where it is. */
	*placed = store_place_item(contact, store_contacts[contact].item_count - 1U);
	return 0;
}

/* Moves an item to its place by its time in its timeline (oldest first; after those of the same time).  Returns its index. */
static size_t
store_place_item(
	long contact,
	size_t item)
{
	struct ph_contact *kept;
	struct ph_item moved;
	size_t at;

	/* Back past the later ones before it. */
	kept = &store_contacts[contact];
	moved = kept->items[item];
	at = item;
	while (at > 0U && kept->items[at - 1U].date > moved.date) {
		kept->items[at] = kept->items[at - 1U];
		at--;
	}

	/* Or, when it did not move back, on past the earlier ones after it. */
	if (at == item) {
		while (at + 1U < kept->item_count && kept->items[at + 1U].date <= moved.date) {
			kept->items[at] = kept->items[at + 1U];
			at++;
		}
	}

	/* Its place. */
	kept->items[at] = moved;
	return at;
}

/* Counts a contact's messages not read, for the list's dot. */
static void
store_count_unread(
	long contact)
{
	struct ph_contact *kept;
	size_t index;

	/* Each unread item. */
	kept = &store_contacts[contact];
	kept->unread = 0U;
	for (index = 0U; index < kept->item_count; index++) {
		if (kept->items[index].state == PH_STATE_UNREAD)
			kept->unread++;
	}
}

/* Copies a line's text with its line ends and other control characters as spaces. */
static void
store_clean_line(
	const char *text,
	char *clean,
	size_t size)
{
	size_t index;

	/* Each byte, while there is room. */
	for (index = 0U; text[index] != '\0' && index + 1U < size; index++) {
		clean[index] = text[index];
		if ((unsigned char)text[index] < 0x20U)
			clean[index] = ' ';
	}

	/* Its end. */
	clean[index] = '\0';
}

/* Names the file of a paired phone's synchronisation.  Returns 0, or EINVAL for an address that is not "AA:BB:CC:DD:EE:FF". */
static int
store_sync_path(
	const char *address,
	char *path,
	size_t size)
{
	size_t length;
	size_t span;

	/* Hexadecimal digits and colons, of an address's length. */
	length = strlen(address);
	span = strspn(address, "0123456789abcdefABCDEF:");
	if (length != 17U || span != length)
		return EINVAL;

	/* Its path. */
	(void)snprintf(path, size, "%s/sync/bt-%s.state", store_root, address);
	return 0;
}
