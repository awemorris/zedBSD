/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * bluetoothd's record of the phone used as a phone (ws197-p003, see
 * phonerec.h).
 */

#include "userland/base/bluetoothd/phonerec.h"
#include "userland/base/bluetoothd/keys.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The version of the record's format. */
#define PHONEREC_VERSION	1UL

/* The largest uid a record holds (a uid of the system's accounts). */
#define PHONEREC_UID_MAX	2147483647UL

/* The longest line read: a key, a space and an account's name. */
#define PHONEREC_LINE_MAX	96U

/* The suffix of a record's file after its bond's name. */
#define PHONEREC_SUFFIX		".phone"

/* The link key types made with a number the user confirmed (Core 5.4 Vol 4 Part E 7.7.24). */
#define PHONEREC_KEY_P192_MITM	0x05U
#define PHONEREC_KEY_P256_MITM	0x08U

/* The keys of a record, by their place in phonerec_keys (each comes once). */
#define PHONEREC_KEY_VERSION	0U
#define PHONEREC_KEY_UID	1U
#define PHONEREC_KEY_USER	2U
#define PHONEREC_KEY_MESSAGES	3U
#define PHONEREC_KEY_CONTACTS	4U
#define PHONEREC_KEY_CALLS	5U
#define PHONEREC_KEY_ENABLED	6U
#define PHONEREC_KEY_ASKED	7U
#define PHONEREC_KEYS		8U

/* The bits of the keys a record must have (every key but "asked", which a record before ws197-p005 lacks). */
#define PHONEREC_KEYS_ALL	0x7fU

/* The keys, in the order of the numbers above. */
static const char *const phonerec_keys[PHONEREC_KEYS] = {
	"version", "uid", "user", "messages", "contacts", "calls", "enabled", "asked",
};

static int phonerec_key(struct btd_phonerec *record, unsigned *seen, const char *key, const char *value);
static int phonerec_number(const char *text, unsigned long maximum, unsigned long *value);
static int phonerec_flag(const char *text, int *flag);
static int phonerec_profiles(const char *text, unsigned *profiles);
static void phonerec_profiles_text(unsigned profiles, char *text, size_t size);
static int phonerec_folder(const char *folder, const uint8_t *controller, char *path, size_t size);
static int phonerec_name_address(const char *name, uint8_t *address);

/* Gives the path of a phone's record (made from the addresses, so nothing outside the folder).  Returns 0 or ENAMETOOLONG. */
int
btd_phonerec_path(
	const char *folder,
	const uint8_t *controller,
	const uint8_t *address,
	char *path,
	size_t size)
{
	char own[24];
	char device[24];
	int written;

	/* Both addresses as text. */
	btd_format_address(controller, own, sizeof(own));
	btd_format_address(address, device, sizeof(device));

	/* The bond's path with the suffix. */
	written = snprintf(path, size, "%s/%s/%s-%s%s", folder, own, device, btd_address_type_name(BTD_ADDRESS_BREDR), PHONEREC_SUFFIX);
	if (written < 0 || (size_t)written >= size)
		return ENAMETOOLONG;

	/* Succeeded. */
	return 0;
}

/*
 * Tells whether an account's name can be kept in a record: 1 to 63
 * characters of letters, digits, dot, underscore and dash.
 */
int
btd_phonerec_user_ok(
	const char *name)
{
	size_t length;
	size_t index;
	char letter;

	/* Its length. */
	length = strlen(name);
	if (length == 0U || length >= BTD_PHONEREC_USER_MAX)
		return 0;

	/* Each character. */
	for (index = 0U; index < length; index++) {
		letter = name[index];
		if (letter >= 'a' && letter <= 'z')
			continue;
		if (letter >= 'A' && letter <= 'Z')
			continue;
		if (letter >= '0' && letter <= '9')
			continue;
		if (letter == '.' || letter == '_' || letter == '-')
			continue;

		/* Any other character. */
		return 0;
	}

	/* A name that can be kept. */
	return 1;
}

/* Writes a record as its text.  Returns 0, EINVAL for a name that cannot be kept, or ENAMETOOLONG. */
int
btd_phonerec_format(
	const struct btd_phonerec *record,
	char *text,
	size_t size)
{
	char asked[8];
	int name_ok;
	int written;

	/* A name the record can keep. */
	name_ok = btd_phonerec_user_ok(record->user);
	if (!name_ok)
		return EINVAL;

	/*
	 * The profiles as asked for: what is on is what was asked for, since
	 * this daemon turns on nothing by itself (ws197-p005 section 8.1).
	 */
	phonerec_profiles_text(record->profiles, asked, sizeof(asked));

	/* The lines, one key each. */
	written = snprintf(text,
			   size,
			   "version %lu\nuid %lu\nuser %s\nmessages %d\ncontacts %d\ncalls %d\nenabled %d\nasked %s\n",
			   PHONEREC_VERSION,
			   (unsigned long)record->uid,
			   record->user,
			   (record->profiles & BTD_PHONEREC_MESSAGES) != 0U,
			   (record->profiles & BTD_PHONEREC_CONTACTS) != 0U,
			   (record->profiles & BTD_PHONEREC_CALLS) != 0U,
			   record->enabled != 0,
			   asked);
	if (written < 0 || (size_t)written >= size)
		return ENAMETOOLONG;

	/* Succeeded: the text. */
	return 0;
}

/*
 * Reads a record's text: every key once, unknown keys passed over.
 * Returns 0 or EBADMSG (the address is not in the text: the caller's).
 */
int
btd_phonerec_parse(
	const char *text,
	size_t length,
	struct btd_phonerec *record)
{
	char line[PHONEREC_LINE_MAX];
	const char *start;
	const char *end;
	char *space;
	size_t line_length;
	unsigned seen;
	int error;

	/* Nothing read yet. */
	memset(record, 0, sizeof(*record));
	seen = 0U;

	/* Each line. */
	start = text;
	while (start < text + length) {
		/* The line, without its newline. */
		end = memchr(start, '\n', (size_t)(text + length - start));
		if (end == NULL)
			end = text + length;
		line_length = (size_t)(end - start);
		if (line_length >= sizeof(line))
			return EBADMSG;
		memcpy(line, start, line_length);
		line[line_length] = '\0';
		start = end + 1;

		/* An empty line is passed over. */
		if (line_length == 0U)
			continue;

		/* Any other is a key, a space and its value. */
		space = strchr(line, ' ');
		if (space == NULL)
			return EBADMSG;
		*space = '\0';
		error = phonerec_key(record, &seen, line, space + 1);
		if (error != 0)
			return error;
	}

	/* Every key a record must have there. */
	if ((seen & PHONEREC_KEYS_ALL) != PHONEREC_KEYS_ALL)
		return EBADMSG;

	/* Succeeded: the record. */
	return 0;
}

/*
 * Writes a record's file whole: a temporary file of 0600 in the
 * controller's folder (made 0700 when it is not there), written, synced
 * and renamed over the old one.  Returns 0 or an errno value.
 */
int
btd_phonerec_write(
	const char *folder,
	const uint8_t *controller,
	const struct btd_phonerec *record)
{
	char text[BTD_PHONEREC_TEXT_MAX];
	char path[256];
	char temporary[264];
	char own_folder[256];
	ssize_t written;
	size_t length;
	int descriptor;
	int named;
	int error;

	/* The text. */
	error = btd_phonerec_format(record, text, sizeof(text));
	if (error != 0)
		return error;
	length = strlen(text);

	/* The controller's folder. */
	error = phonerec_folder(folder, controller, own_folder, sizeof(own_folder));
	if (error != 0)
		return error;

	/* The record's path. */
	error = btd_phonerec_path(folder, controller, record->address, path, sizeof(path));
	if (error != 0)
		return error;

	/* The temporary file's path beside it. */
	named = snprintf(temporary, sizeof(temporary), "%s.tmp", path);
	if (named < 0 || (size_t)named >= sizeof(temporary))
		return ENAMETOOLONG;

	/* The temporary file (never a link). */
	descriptor = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (descriptor < 0)
		return errno;

	/* The text written. */
	written = write(descriptor, text, length);
	if (written != (ssize_t)length) {
		error = EIO;
		if (written < 0)
			error = errno;
		(void)close(descriptor);
		return error;
	}

	/* The bytes on the disk before the rename. */
	error = fsync(descriptor);
	if (error != 0) {
		error = errno;
		(void)close(descriptor);
		return error;
	}

	/* Written. */
	(void)close(descriptor);

	/* In place of the old one. */
	error = rename(temporary, path);
	if (error != 0)
		return errno;

	/* The folder synced, so the rename stays (a failure leaves the file as it is). */
	descriptor = open(own_folder, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (descriptor >= 0) {
		(void)fsync(descriptor);
		(void)close(descriptor);
	}

	/* Succeeded: the record is on disk. */
	return 0;
}

/*
 * Reads a phone's record.  Returns 0, ENOENT when there is none, EBADMSG
 * for a malformed one, or another errno value.
 */
int
btd_phonerec_read(
	const char *folder,
	const uint8_t *controller,
	const uint8_t *address,
	struct btd_phonerec *record)
{
	char text[BTD_PHONEREC_TEXT_MAX];
	char path[256];
	ssize_t got;
	int descriptor;
	int error;

	/* The file. */
	error = btd_phonerec_path(folder, controller, address, path, sizeof(path));
	if (error != 0)
		return error;
	descriptor = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (descriptor < 0)
		return errno;

	/* Read whole. */
	got = read(descriptor, text, sizeof(text));
	(void)close(descriptor);

	/* A file that could not be read is refused. */
	if (got < 0)
		return EIO;

	/* A file as long as the buffer may be cut: refused. */
	if ((size_t)got >= sizeof(text))
		return EBADMSG;

	/* Its fields. */
	error = btd_phonerec_parse(text, (size_t)got, record);
	if (error != 0)
		return error;

	/* Succeeded: the record, for the phone the path names. */
	memcpy(record->address, address, BTD_ADDRESS_BYTES);
	return 0;
}

/* Removes a phone's record.  Returns 0, ENOENT, or another errno value. */
int
btd_phonerec_forget(
	const char *folder,
	const uint8_t *controller,
	const uint8_t *address)
{
	char path[256];
	int error;

	/* The file's path. */
	error = btd_phonerec_path(folder, controller, address, path, sizeof(path));
	if (error != 0)
		return error;

	/* Gone. */
	error = unlink(path);
	if (error != 0)
		return errno;

	/* Succeeded. */
	return 0;
}

/*
 * Reads every record of the controller's folder that reads at all (a
 * malformed file is passed over), max at most.  Returns 0, or the error
 * of reading the folder (none is no record).
 */
int
btd_phonerec_list(
	const char *folder,
	const uint8_t *controller,
	struct btd_phonerec *records,
	unsigned max,
	unsigned *count)
{
	struct dirent *entry;
	uint8_t address[BTD_ADDRESS_BYTES];
	char own[24];
	char path[256];
	DIR *directory;
	int written;
	int error;

	/* The controller's folder; none is no record. */
	*count = 0U;
	btd_format_address(controller, own, sizeof(own));
	written = snprintf(path, sizeof(path), "%s/%s", folder, own);
	if (written < 0 || (size_t)written >= sizeof(path))
		return ENAMETOOLONG;
	directory = opendir(path);
	if (directory == NULL) {
		error = errno;
		if (error == ENOENT)
			return 0;
		return error;
	}

	/* Each file named ADDRESS-bredr.phone. */
	for (;;) {
		entry = readdir(directory);
		if (entry == NULL)
			break;
		if (*count >= max)
			break;

		/* The phone's address in the name. */
		error = phonerec_name_address(entry->d_name, address);
		if (error != 0)
			continue;

		/* The record, read and checked like any other. */
		error = btd_phonerec_read(folder, controller, address, &records[*count]);
		if (error != 0)
			continue;
		*count += 1U;
	}

	/* The folder is read. */
	(void)closedir(directory);

	/* Succeeded: the records found. */
	return 0;
}

/*
 * Tells whether a record is valid: its bond is there with a key made with
 * a number the user confirmed, and its uid's account has the name
 * written (an account removed, or its uid given to another, owns
 * nothing).  Returns 1 or 0.
 */
int
btd_phonerec_valid(
	const char *folder,
	const uint8_t *controller,
	const struct btd_phonerec *record,
	btd_phonerec_account_fn account,
	void *context)
{
	struct btd_bond bond;
	char name[BTD_PHONEREC_USER_MAX];
	int authenticated;
	int same;
	int error;

	/* The bond. */
	error = btd_keys_read(folder, controller, record->address, BTD_ADDRESS_BREDR, &bond);
	if (error != 0)
		return 0;

	/* Its key, made with a number the user confirmed: an authenticated P-192 or P-256 key. */
	authenticated = 0;
	if (bond.have_link_key && bond.link_key_type == PHONEREC_KEY_P192_MITM)
		authenticated = 1;
	if (bond.have_link_key && bond.link_key_type == PHONEREC_KEY_P256_MITM)
		authenticated = 1;

	/* The keys are not kept; a bond of another key owns nothing. */
	memset(&bond, 0, sizeof(bond));
	if (!authenticated)
		return 0;

	/* The owner's account. */
	error = account(context, record->uid, name, sizeof(name));
	if (error != 0)
		return 0;

	/* With the name written. */
	same = strcmp(name, record->user);
	if (same != 0)
		return 0;

	/* Valid. */
	return 1;
}

/* How many records one pruning looks at. */
#define PHONEREC_PRUNE_MAX	16U

/*
 * Removes the records of the controller's folder whose bond is not there
 * (a bond forgotten while the daemon did not run, section 3.1).  Returns
 * 0, or the error of reading the folder.
 */
int
btd_phonerec_prune(
	const char *folder,
	const uint8_t *controller)
{
	uint8_t addresses[PHONEREC_PRUNE_MAX][BTD_ADDRESS_BYTES];
	struct dirent *entry;
	struct stat status;
	char own[24];
	char path[256];
	char bond_path[256];
	DIR *directory;
	unsigned found;
	unsigned index;
	int written;
	int error;

	/* The controller's folder; none is nothing to prune. */
	btd_format_address(controller, own, sizeof(own));
	written = snprintf(path, sizeof(path), "%s/%s", folder, own);
	if (written < 0 || (size_t)written >= sizeof(path))
		return ENAMETOOLONG;
	directory = opendir(path);
	if (directory == NULL) {
		error = errno;
		if (error == ENOENT)
			return 0;
		return error;
	}

	/* Each record whose bond's file is not there, noted (removed after the folder is read). */
	found = 0U;
	for (;;) {
		entry = readdir(directory);
		if (entry == NULL || found >= PHONEREC_PRUNE_MAX)
			break;

		/* The phone's address in the name. */
		error = phonerec_name_address(entry->d_name, addresses[found]);
		if (error != 0)
			continue;

		/* Its bond's file. */
		error = btd_keys_path(folder, controller, addresses[found], BTD_ADDRESS_BREDR, bond_path, sizeof(bond_path));
		if (error != 0)
			continue;
		error = stat(bond_path, &status);
		if (error == 0)
			continue;

		/* None: noted. */
		found++;
	}

	/* The folder is read. */
	(void)closedir(directory);

	/* Succeeded: the records without a bond go. */
	for (index = 0U; index < found; index++)
		(void)btd_phonerec_forget(folder, controller, addresses[index]);
	return 0;
}

/*
 * Brings a record written before ws197-p005 up to date (section 8.1): its
 * pairing turned every profile on by itself, so contacts and calls go
 * off and only messages stays as it was; the "asked" line is then
 * written with what is left.  The user turns contacts on again from
 * Settings or with LINK.  Returns 1 when the record changed (the caller
 * writes it), 0 for a record already up to date.
 */
int
btd_phonerec_migrate(
	struct btd_phonerec *record)
{
	/* Up to date already. */
	if (record->have_asked)
		return 0;

	/* Succeeded: messages alone kept, the rest off, as asked from now on. */
	record->profiles &= BTD_PHONEREC_MESSAGES;
	record->asked = record->profiles;
	record->have_asked = 1;
	return 1;
}

/* Reads one line of a record; a known key may come once, an unknown one is passed over.  Returns 0 or EBADMSG. */
static int
phonerec_key(
	struct btd_phonerec *record,
	unsigned *seen,
	const char *key,
	const char *value)
{
	unsigned long number;
	unsigned index;
	int name_ok;
	int flag;
	int same;
	int error;

	/* Which key. */
	for (index = 0U; index < PHONEREC_KEYS; index++) {
		same = strcmp(key, phonerec_keys[index]);
		if (same == 0)
			break;
	}

	/* An unknown key, from a later format. */
	if (index == PHONEREC_KEYS)
		return 0;

	/* A key seen before. */
	if ((*seen & (1U << index)) != 0U)
		return EBADMSG;
	*seen |= 1U << index;

	/* The account's name. */
	if (index == PHONEREC_KEY_USER) {
		name_ok = btd_phonerec_user_ok(value);
		if (!name_ok)
			return EBADMSG;
		(void)snprintf(record->user, sizeof(record->user), "%s", value);
		return 0;
	}

	/* The format's version, which must be this one. */
	if (index == PHONEREC_KEY_VERSION) {
		error = phonerec_number(value, PHONEREC_VERSION, &number);
		if (error != 0 || number != PHONEREC_VERSION)
			return EBADMSG;
		return 0;
	}

	/* The profiles asked for. */
	if (index == PHONEREC_KEY_ASKED) {
		error = phonerec_profiles(value, &record->asked);
		if (error != 0)
			return EBADMSG;
		record->have_asked = 1;
		return 0;
	}

	/* The owner's uid. */
	if (index == PHONEREC_KEY_UID) {
		error = phonerec_number(value, PHONEREC_UID_MAX, &number);
		if (error != 0)
			return EBADMSG;
		record->uid = (uid_t)number;
		return 0;
	}

	/* A switch, 0 or 1. */
	error = phonerec_flag(value, &flag);
	if (error != 0)
		return EBADMSG;

	/* Which switch. */
	switch (index) {
	case PHONEREC_KEY_MESSAGES:
		if (flag)
			record->profiles |= BTD_PHONEREC_MESSAGES;
		break;
	case PHONEREC_KEY_CONTACTS:
		if (flag)
			record->profiles |= BTD_PHONEREC_CONTACTS;
		break;
	case PHONEREC_KEY_CALLS:
		if (flag)
			record->profiles |= BTD_PHONEREC_CALLS;
		break;
	default:
		record->enabled = flag;
		break;
	}

	/* Succeeded: the switch kept. */
	return 0;
}

/* Reads a decimal number of digits alone, no larger than maximum.  Returns 0 or EBADMSG. */
static int
phonerec_number(
	const char *text,
	unsigned long maximum,
	unsigned long *value)
{
	unsigned long number;
	size_t length;
	size_t index;

	/* At least one digit, at most ten. */
	length = strlen(text);
	if (length == 0U || length > 10U)
		return EBADMSG;

	/* Each digit. */
	number = 0UL;
	for (index = 0U; text[index] != '\0'; index++) {
		if (text[index] < '0' || text[index] > '9')
			return EBADMSG;
		number = number * 10UL + (unsigned long)(text[index] - '0');
	}

	/* No larger than allowed. */
	if (number > maximum)
		return EBADMSG;

	/* Succeeded: the number. */
	*value = number;
	return 0;
}

/* Reads a switch: "0" or "1".  Returns 0 or EBADMSG. */
static int
phonerec_flag(
	const char *text,
	int *flag)
{
	int same;

	/* Off. */
	same = strcmp(text, "0");
	if (same == 0) {
		*flag = 0;
		return 0;
	}

	/* On. */
	same = strcmp(text, "1");
	if (same == 0) {
		*flag = 1;
		return 0;
	}

	/* Anything else. */
	return EBADMSG;
}

/* Makes the controller's folder (0700) when it is not there, and gives its path. */
static int
phonerec_folder(
	const char *folder,
	const uint8_t *controller,
	char *path,
	size_t size)
{
	char own[24];
	int written;
	int error;

	/* The path. */
	btd_format_address(controller, own, sizeof(own));
	written = snprintf(path, size, "%s/%s", folder, own);
	if (written < 0 || (size_t)written >= size)
		return ENAMETOOLONG;

	/* Made, or there already. */
	error = mkdir(path, 0700);
	if (error != 0 && errno != EEXIST)
		return errno;

	/* Succeeded. */
	return 0;
}

/* Reads the address of a record's file name, ADDRESS-bredr.phone.  Returns 0 or EINVAL. */
static int
phonerec_name_address(
	const char *name,
	uint8_t *address)
{
	char address_text[18];
	char expected[32];
	size_t length;
	int written;
	int same;
	int error;

	/* Seventeen characters of the address and the rest. */
	length = strlen(name);
	if (length <= 17U || name[0] == '.')
		return EINVAL;
	memcpy(address_text, name, 17U);
	address_text[17] = '\0';
	error = btd_address_parse(address_text, address);
	if (error != 0)
		return EINVAL;

	/* The rest: the BR/EDR type and the suffix. */
	written = snprintf(expected, sizeof(expected), "-%s%s", btd_address_type_name(BTD_ADDRESS_BREDR), PHONEREC_SUFFIX);
	if (written < 0 || (size_t)written >= sizeof(expected))
		return EINVAL;
	same = strcmp(name + 17, expected);
	if (same != 0)
		return EINVAL;

	/* Succeeded: the address. */
	return 0;
}

/*
 * Reads the profiles of an "asked" line: "-" for none, else the letters m
 * (messages), c (contacts) and h (calls) with ',' between them, each
 * once.  Returns 0 or EBADMSG.
 */
static int
phonerec_profiles(
	const char *text,
	unsigned *profiles)
{
	size_t index;
	unsigned bit;

	/* None. */
	*profiles = 0U;
	if (text[0] == '-' && text[1] == '\0')
		return 0;

	/* Each letter, a ',' between two. */
	for (index = 0U; text[index] != '\0'; index++) {
		/* A ',' only between two letters. */
		if ((index & 1U) != 0U) {
			if (text[index] != ',' || text[index + 1U] == '\0')
				return EBADMSG;
			continue;
		}

		/* The letter's profile. */
		bit = 0U;
		if (text[index] == 'm') {
			bit = BTD_PHONEREC_MESSAGES;
		} else if (text[index] == 'c') {
			bit = BTD_PHONEREC_CONTACTS;
		} else if (text[index] == 'h') {
			bit = BTD_PHONEREC_CALLS;
		}

		/* A letter not known, or one given twice. */
		if (bit == 0U || (*profiles & bit) != 0U)
			return EBADMSG;
		*profiles |= bit;
	}

	/* An empty value. */
	if (index == 0U)
		return EBADMSG;

	/* Succeeded: the profiles. */
	return 0;
}

/* Writes profiles as an "asked" value: "m,c,h" (each letter when on), or "-" for none. */
static void
phonerec_profiles_text(
	unsigned profiles,
	char *text,
	size_t size)
{
	static const unsigned bits[3] = { BTD_PHONEREC_MESSAGES, BTD_PHONEREC_CONTACTS, BTD_PHONEREC_CALLS };
	static const char letters[3] = { 'm', 'c', 'h' };
	size_t used;
	size_t index;

	/* Each profile on, a ',' before all but the first (the room holds "m,c,h" and its NUL). */
	used = 0U;
	for (index = 0U; index < 3U && used + 3U <= size; index++) {
		/* A profile that is off. */
		if ((profiles & bits[index]) == 0U)
			continue;

		/* The ',' and the letter. */
		if (used != 0U) {
			text[used] = ',';
			used++;
		}

		/* The letter. */
		text[used] = letters[index];
		used++;
	}

	/* None on. */
	if (used == 0U) {
		text[used] = '-';
		used++;
	}

	/* The end. */
	text[used] = '\0';
}
