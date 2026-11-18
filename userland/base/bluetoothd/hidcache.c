/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * bluetoothd's records of HID devices (ws143-p005 i02, see hidcache.h).
 */

#include "userland/base/bluetoothd/hidcache.h"
#include "userland/base/bluetoothd/keys.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The descriptor's most lines. */
#define HIDCACHE_LINES		(BTD_HIDCACHE_DESCRIPTOR_MAX / BTD_HIDCACHE_LINE_BYTES)

/* The longest line read (a descriptor line's key and 256 hex digits, or an escaped name). */
#define HIDCACHE_LINE_MAX	(4U * BTD_NAME_MAX + 32U)

/* The keys of a record, by their place in hidcache_keys (each may come once). */
#define HIDCACHE_KEY_STATE		0U
#define HIDCACHE_KEY_TRANSPORT		1U
#define HIDCACHE_KEY_RECONNECT		2U
#define HIDCACHE_KEY_CONNECTABLE	3U
#define HIDCACHE_KEY_CABLE		4U
#define HIDCACHE_KEY_BOOT		5U
#define HIDCACHE_KEY_VENDOR		6U
#define HIDCACHE_KEY_PRODUCT		7U
#define HIDCACHE_KEY_VERSION		8U
#define HIDCACHE_KEY_COUNTRY		9U
#define HIDCACHE_KEY_CLASS		10U
#define HIDCACHE_KEY_APPEARANCE		11U
#define HIDCACHE_KEY_NAME		12U
#define HIDCACHE_KEY_SIZE		13U
#define HIDCACHE_KEYS			14U

/* The keys, in the order of the numbers above. */
static const char *const hidcache_keys[HIDCACHE_KEYS] = {
	"state", "transport", "reconnect_initiate", "normally_connectable", "virtual_cable", "boot_device",
	"vendor", "product", "version", "country", "class", "appearance", "name", "descriptor_size",
};

/* What a parse has seen: the keys and the descriptor's lines, each once. */
struct hidcache_seen {
	uint32_t keys;
	uint32_t lines;
	uint8_t counts[HIDCACHE_LINES];
};

static int hidcache_key(struct btd_hidcache *record, struct hidcache_seen *seen, const char *key, const char *value);
static int hidcache_line(struct btd_hidcache *record, struct hidcache_seen *seen, unsigned index, const char *value);
static int hidcache_number(const char *text, unsigned long maximum, int hex, unsigned long *value);
static int hidcache_folder(const char *folder, const uint8_t *controller, char *path, size_t size);
static int hidcache_put(char *text, size_t size, size_t *used, const char *format, unsigned long value);

/*
 * Writes the path of a record's file.  Returns 0, or ENAMETOOLONG.
 */
int
btd_hidcache_path(
	const char *folder,
	const uint8_t *controller,
	const uint8_t *address,
	unsigned type,
	char *path,
	size_t size)
{
	char own[24];
	char device[24];
	int written;

	/* Both addresses as text (hex digits and colons only: nothing that could leave the folder). */
	btd_format_address(controller, own, sizeof(own));
	btd_format_address(address, device, sizeof(device));

	/* The path, the bond's with ".hid". */
	written = snprintf(path, size, "%s/%s/%s-%s.hid", folder, own, device, btd_address_type_name(type));
	if (written < 0 || (size_t)written >= size)
		return ENAMETOOLONG;

	/* Succeeded. */
	return 0;
}

/*
 * Writes a record as its file's text.  Returns 0, EINVAL for a descriptor
 * past the most, or ENAMETOOLONG.
 */
int
btd_hidcache_format(
	const struct btd_hidcache *record,
	char *text,
	size_t size)
{
	char name[4U * BTD_NAME_MAX];
	const char *state;
	const char *transport;
	size_t used;
	size_t line;
	size_t start;
	size_t count;
	size_t index;
	int written;
	int error;

	/* Refuses a descriptor past the most. */
	if (record->descriptor_size > BTD_HIDCACHE_DESCRIPTOR_MAX)
		return EINVAL;

	/* The state, the transport and the name. */
	error = btd_escape(record->name, name, sizeof(name));
	if (error != 0)
		name[0] = '\0';
	state = "candidate";
	if (record->confirmed)
		state = "confirmed";
	transport = "bredr";
	if (record->le)
		transport = "le";
	written = snprintf(text, size, "state=%s\ntransport=%s\nname=%s\n", state, transport, name);
	if (written < 0 || (size_t)written >= size)
		return ENAMETOOLONG;
	used = (size_t)written;

	/* The flags and numbers. */
	error = hidcache_put(text, size, &used, "reconnect_initiate=%lu\n", (unsigned long)(record->reconnect_initiate != 0));
	if (error == 0)
		error = hidcache_put(text, size, &used, "normally_connectable=%lu\n", (unsigned long)(record->normally_connectable != 0));
	if (error == 0)
		error = hidcache_put(text, size, &used, "virtual_cable=%lu\n", (unsigned long)(record->virtual_cable != 0));
	if (error == 0)
		error = hidcache_put(text, size, &used, "boot_device=%lu\n", (unsigned long)(record->boot_device != 0));
	if (error == 0)
		error = hidcache_put(text, size, &used, "vendor=%04lx\n", (unsigned long)record->vendor);
	if (error == 0)
		error = hidcache_put(text, size, &used, "product=%04lx\n", (unsigned long)record->product);
	if (error == 0)
		error = hidcache_put(text, size, &used, "version=%04lx\n", (unsigned long)record->version);
	if (error == 0)
		error = hidcache_put(text, size, &used, "country=%lu\n", (unsigned long)record->country);
	if (error == 0)
		error = hidcache_put(text, size, &used, "class=%06lx\n", (unsigned long)record->device_class);
	if (error == 0)
		error = hidcache_put(text, size, &used, "appearance=%04lx\n", (unsigned long)record->appearance);
	if (error == 0)
		error = hidcache_put(text, size, &used, "descriptor_size=%lu\n", (unsigned long)record->descriptor_size);
	if (error != 0)
		return error;

	/* The descriptor, 128 bytes a line. */
	for (line = 0; line * BTD_HIDCACHE_LINE_BYTES < record->descriptor_size; line++) {
		start = line * BTD_HIDCACHE_LINE_BYTES;
		count = record->descriptor_size - start;
		if (count > BTD_HIDCACHE_LINE_BYTES)
			count = BTD_HIDCACHE_LINE_BYTES;
		written = snprintf(text + used, size - used, "descriptor_%lu=", (unsigned long)line);
		if (written < 0 || (size_t)written >= size - used)
			return ENAMETOOLONG;
		used += (size_t)written;
		if (2U * count + 2U > size - used)
			return ENAMETOOLONG;
		for (index = 0; index < count; index++) {
			(void)snprintf(text + used, 3U, "%02x", (unsigned)record->descriptor[start + index]);
			used += 2U;
		}

		/* The line ends. */
		text[used] = '\n';
		used++;
		text[used] = '\0';
	}

	/* Succeeded: the text. */
	return 0;
}

/*
 * Reads a record file's text.  Returns 0, or EBADMSG for a line without
 * '=', a key repeated or malformed, no state or transport, or a
 * descriptor whose lines are missing, repeated or not its size.
 */
int
btd_hidcache_parse(
	const char *text,
	size_t length,
	struct btd_hidcache *record)
{
	struct hidcache_seen seen;
	char line[HIDCACHE_LINE_MAX];
	const char *start;
	const char *end;
	char *equals;
	size_t line_length;
	unsigned long index;
	uint32_t wanted;
	size_t lines;
	size_t expected;
	int descriptor_line;
	int error;

	/* Nothing read yet. */
	memset(record, 0, sizeof(*record));
	memset(&seen, 0, sizeof(seen));

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

		/* An empty line is passed over; any other is key=value. */
		if (line_length == 0U)
			continue;
		equals = strchr(line, '=');
		if (equals == NULL)
			return EBADMSG;
		*equals = '\0';

		/* A descriptor's line, or another key. */
		descriptor_line = strncmp(line, "descriptor_", 11) == 0;
		if (descriptor_line)
			descriptor_line = strcmp(line, "descriptor_size") != 0;
		if (descriptor_line) {
			error = hidcache_number(line + 11, HIDCACHE_LINES - 1U, 0, &index);
			if (error != 0)
				return EBADMSG;
			error = hidcache_line(record, &seen, (unsigned)index, equals + 1);
			if (error != 0)
				return error;
			continue;
		}

		/* Another key. */
		error = hidcache_key(record, &seen, line, equals + 1);
		if (error != 0)
			return error;
	}

	/* A record names its state and transport. */
	wanted = (1U << HIDCACHE_KEY_STATE) | (1U << HIDCACHE_KEY_TRANSPORT) | (1U << HIDCACHE_KEY_SIZE);
	if ((seen.keys & wanted) != wanted)
		return EBADMSG;

	/* Every line of the descriptor, and no other. */
	lines = (record->descriptor_size + BTD_HIDCACHE_LINE_BYTES - 1U) / BTD_HIDCACHE_LINE_BYTES;
	wanted = 0;
	if (lines >= 32U)
		wanted = 0xffffffffU;
	else if (lines != 0U)
		wanted = (1U << lines) - 1U;
	if (seen.lines != wanted)
		return EBADMSG;

	/* Every line full but the last, which ends at the size. */
	for (index = 0; index < lines; index++) {
		expected = BTD_HIDCACHE_LINE_BYTES;
		if (index + 1U == lines)
			expected = record->descriptor_size - (lines - 1U) * BTD_HIDCACHE_LINE_BYTES;
		if ((size_t)seen.counts[index] != expected)
			return EBADMSG;
	}

	/* Succeeded: the record. */
	return 0;
}

/*
 * Writes a record's file whole: a temporary file of 0600 in the
 * controller's folder (made 0700 when it is not there), written, synced
 * and renamed over the old one.  Returns 0 or an errno value.
 */
int
btd_hidcache_write(
	const char *folder,
	const uint8_t *controller,
	const struct btd_hidcache *record)
{
	char path[256];
	char temporary[264];
	char own_folder[256];
	char *text;
	ssize_t written;
	size_t length;
	int descriptor;
	int written_name;
	int error;

	/* The text. */
	text = malloc(BTD_HIDCACHE_TEXT_MAX);
	if (text == NULL)
		return ENOMEM;
	error = btd_hidcache_format(record, text, BTD_HIDCACHE_TEXT_MAX);
	if (error != 0) {
		free(text);
		return error;
	}

	/* Its length. */
	length = strlen(text);

	/* The folder and the paths. */
	error = hidcache_folder(folder, controller, own_folder, sizeof(own_folder));
	if (error == 0)
		error = btd_hidcache_path(folder, controller, record->address, record->type, path, sizeof(path));
	written_name = snprintf(temporary, sizeof(temporary), "%s.tmp", path);
	if (error == 0 && (written_name < 0 || (size_t)written_name >= sizeof(temporary)))
		error = ENAMETOOLONG;
	if (error != 0) {
		free(text);
		return error;
	}

	/* The temporary file (never a link), written and synced. */
	descriptor = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (descriptor < 0) {
		error = errno;
		free(text);
		return error;
	}

	/* The text written. */
	written = write(descriptor, text, length);
	free(text);
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

	/* In place of the old one, and the folder synced. */
	error = rename(temporary, path);
	if (error != 0)
		return errno;
	descriptor = open(own_folder, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (descriptor >= 0) {
		(void)fsync(descriptor);
		(void)close(descriptor);
	}

	/* Succeeded: the record is on disk. */
	return 0;
}

/*
 * Reads a record's file.  Returns 0, ENOENT when there is none, EBADMSG
 * for a malformed one, or another errno value.
 */
int
btd_hidcache_read(
	const char *folder,
	const uint8_t *controller,
	const uint8_t *address,
	unsigned type,
	struct btd_hidcache *record)
{
	char path[256];
	char *text;
	ssize_t got;
	int descriptor;
	int error;

	/* The file. */
	error = btd_hidcache_path(folder, controller, address, type, path, sizeof(path));
	if (error != 0)
		return error;
	descriptor = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (descriptor < 0)
		return errno;
	text = malloc(BTD_HIDCACHE_TEXT_MAX);
	if (text == NULL) {
		(void)close(descriptor);
		return ENOMEM;
	}

	/* Read whole. */
	got = read(descriptor, text, BTD_HIDCACHE_TEXT_MAX);
	(void)close(descriptor);

	/* A file that could not be read is refused. */
	if (got < 0) {
		free(text);
		return EIO;
	}

	/* A file as long as the buffer may be cut: refused. */
	if ((size_t)got >= BTD_HIDCACHE_TEXT_MAX) {
		free(text);
		return EBADMSG;
	}

	/* Its fields, for the device the path names. */
	error = btd_hidcache_parse(text, (size_t)got, record);
	free(text);
	if (error != 0)
		return error;
	memcpy(record->address, address, BTD_ADDRESS_BYTES);
	record->type = type;

	/* Succeeded: the record. */
	return 0;
}

/*
 * Removes a record's file.  Returns 0, ENOENT, or another errno value.
 */
int
btd_hidcache_forget(
	const char *folder,
	const uint8_t *controller,
	const uint8_t *address,
	unsigned type)
{
	char path[256];
	int error;

	/* The file's path (made from the addresses, so nothing outside the folder). */
	error = btd_hidcache_path(folder, controller, address, type, path, sizeof(path));
	if (error != 0)
		return error;

	/* Gone. */
	error = unlink(path);
	if (error != 0)
		return errno;

	/* Succeeded. */
	return 0;
}

/* How many records one pruning looks at. */
#define HIDCACHE_PRUNE_MAX	32U

/*
 * Removes the records of the controller's folder whose bond is not among
 * the bonds given (a bond forgotten while the daemon did not run,
 * phase005 section 9.3).  Returns 0, or the error of reading the folder.
 */
int
btd_hidcache_prune(
	const char *folder,
	const uint8_t *controller,
	const struct btd_bond *bonds,
	unsigned count)
{
	uint8_t addresses[HIDCACHE_PRUNE_MAX][BTD_ADDRESS_BYTES];
	unsigned types[HIDCACHE_PRUNE_MAX];
	struct dirent *entry;
	char own[24];
	char path[256];
	char address_text[18];
	char type_text[16];
	const char *dash;
	const char *dot;
	unsigned found;
	unsigned index;
	unsigned bond;
	unsigned type;
	int bonded;
	int same;
	int written;
	int error;
	DIR *directory;

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

	/* Each file named ADDRESS-TYPE.hid without its bond, noted (removed after the folder is read). */
	found = 0U;
	for (;;) {
		entry = readdir(directory);
		if (entry == NULL || found >= HIDCACHE_PRUNE_MAX)
			break;

		/* The address, 17 characters before the dash, and the type up to ".hid". */
		dash = strchr(entry->d_name, '-');
		dot = strrchr(entry->d_name, '.');
		if (entry->d_name[0] == '.' || dash == NULL || dash - entry->d_name != 17 || dot == NULL || dot < dash)
			continue;
		same = strcmp(dot, ".hid");
		if (same != 0 || (size_t)(dot - dash - 1) >= sizeof(type_text))
			continue;
		memcpy(address_text, entry->d_name, 17U);
		address_text[17] = '\0';
		memcpy(type_text, dash + 1, (size_t)(dot - dash - 1));
		type_text[dot - dash - 1] = '\0';
		error = btd_address_parse(address_text, addresses[found]);
		if (error != 0)
			continue;
		error = btd_address_type_parse(type_text, &type);
		if (error != 0)
			continue;

		/* Its bond, when there is one. */
		bonded = 0;
		for (bond = 0U; bond < count; bond++) {
			same = memcmp(bonds[bond].address, addresses[found], BTD_ADDRESS_BYTES);
			if (same == 0 && bonds[bond].type == type)
				bonded = 1;
		}

		/* None: noted. */
		if (!bonded) {
			types[found] = type;
			found++;
		}
	}

	/* The folder is read. */
	(void)closedir(directory);

	/* Succeeded: the records without a bond go. */
	for (index = 0U; index < found; index++)
		(void)btd_hidcache_forget(folder, controller, addresses[index], types[index]);
	return 0;
}

/* Reads one key of a record; a key may come once, an unknown one is passed over.  Returns 0 or EBADMSG. */
static int
hidcache_key(
	struct btd_hidcache *record,
	struct hidcache_seen *seen,
	const char *key,
	const char *value)
{
	unsigned long number;
	unsigned field;
	int different;
	int first;
	int second;
	int error;

	/* The key's place, or none. */
	for (field = 0; field < HIDCACHE_KEYS; field++) {
		different = strcmp(key, hidcache_keys[field]);
		if (different == 0)
			break;
	}

	/* An unknown key is passed over. */
	if (field == HIDCACHE_KEYS)
		return 0;

	/* Refuses a key that came before. */
	if ((seen->keys & (1U << field)) != 0U)
		return EBADMSG;
	seen->keys |= 1U << field;

	/* The state: confirmed or a candidate. */
	if (field == HIDCACHE_KEY_STATE) {
		first = strcmp(value, "confirmed");
		second = strcmp(value, "candidate");
		if (first != 0 && second != 0)
			return EBADMSG;
		record->confirmed = first == 0;
		return 0;
	}

	/* The transport: LE or BR/EDR. */
	if (field == HIDCACHE_KEY_TRANSPORT) {
		first = strcmp(value, "le");
		second = strcmp(value, "bredr");
		if (first != 0 && second != 0)
			return EBADMSG;
		record->le = first == 0;
		return 0;
	}

	/* The name, kept escaped as written. */
	if (field == HIDCACHE_KEY_NAME) {
		(void)snprintf(record->name, sizeof(record->name), "%s", value);
		return 0;
	}

	/* The numbers: flags 0 or 1, the PnP numbers and the appearance in hex, the class in hex, the size and the country. */
	error = 0;
	number = 0;
	switch (field) {
	case HIDCACHE_KEY_RECONNECT:
	case HIDCACHE_KEY_CONNECTABLE:
	case HIDCACHE_KEY_CABLE:
	case HIDCACHE_KEY_BOOT:
		error = hidcache_number(value, 1UL, 0, &number);
		break;
	case HIDCACHE_KEY_VENDOR:
	case HIDCACHE_KEY_PRODUCT:
	case HIDCACHE_KEY_VERSION:
	case HIDCACHE_KEY_APPEARANCE:
		error = hidcache_number(value, 0xffffUL, 1, &number);
		break;
	case HIDCACHE_KEY_CLASS:
		error = hidcache_number(value, 0xffffffUL, 1, &number);
		break;
	case HIDCACHE_KEY_COUNTRY:
		error = hidcache_number(value, 0xffUL, 0, &number);
		break;
	default:
		error = hidcache_number(value, BTD_HIDCACHE_DESCRIPTOR_MAX, 0, &number);
		break;
	}

	/* Refuses a number malformed or past its most. */
	if (error != 0)
		return EBADMSG;

	/* Kept in its field. */
	if (field == HIDCACHE_KEY_RECONNECT)
		record->reconnect_initiate = (int)number;
	else if (field == HIDCACHE_KEY_CONNECTABLE)
		record->normally_connectable = (int)number;
	else if (field == HIDCACHE_KEY_CABLE)
		record->virtual_cable = (int)number;
	else if (field == HIDCACHE_KEY_BOOT)
		record->boot_device = (int)number;
	else if (field == HIDCACHE_KEY_VENDOR)
		record->vendor = (uint16_t)number;
	else if (field == HIDCACHE_KEY_PRODUCT)
		record->product = (uint16_t)number;
	else if (field == HIDCACHE_KEY_VERSION)
		record->version = (uint16_t)number;
	else if (field == HIDCACHE_KEY_APPEARANCE)
		record->appearance = (uint16_t)number;
	else if (field == HIDCACHE_KEY_CLASS)
		record->device_class = (uint32_t)number;
	else if (field == HIDCACHE_KEY_COUNTRY)
		record->country = (uint8_t)number;
	else
		record->descriptor_size = (size_t)number;

	/* Succeeded. */
	return 0;
}

/*
 * Reads one line of the descriptor: whole bytes in hex, 128 of them
 * unless it is the last line (the size is checked once all are read).
 * Returns 0 or EBADMSG.
 */
static int
hidcache_line(
	struct btd_hidcache *record,
	struct hidcache_seen *seen,
	unsigned index,
	const char *value)
{
	size_t length;
	size_t count;
	size_t byte;
	unsigned long number;
	char pair[3];
	int error;

	/* Refuses a line that came before, or of no whole bytes or more than a line's. */
	if ((seen->lines & (1U << index)) != 0U)
		return EBADMSG;
	seen->lines |= 1U << index;
	length = strlen(value);
	if (length == 0U || length % 2U != 0U || length / 2U > BTD_HIDCACHE_LINE_BYTES)
		return EBADMSG;
	count = length / 2U;

	/* Its length (1 to 128), for the parse to check against the size. */
	seen->counts[index] = (uint8_t)count;

	/* Each byte. */
	pair[2] = '\0';
	for (byte = 0; byte < count; byte++) {
		pair[0] = value[2U * byte];
		pair[1] = value[2U * byte + 1U];
		error = hidcache_number(pair, 0xffUL, 1, &number);
		if (error != 0)
			return EBADMSG;
		record->descriptor[index * BTD_HIDCACHE_LINE_BYTES + byte] = (uint8_t)number;
	}

	/* Succeeded. */
	return 0;
}

/* Reads a whole number, decimal or hex, at most the maximum.  Returns 0 or EINVAL. */
static int
hidcache_number(
	const char *text,
	unsigned long maximum,
	int hex,
	unsigned long *value)
{
	char *end;
	int base;

	/* Refuses an empty text or a sign. */
	if (text[0] == '\0' || text[0] == '-' || text[0] == '+' || text[0] == ' ')
		return EINVAL;

	/* The number, all of the text, within the maximum. */
	base = 10;
	if (hex)
		base = 16;
	errno = 0;
	*value = strtoul(text, &end, base);
	if (errno != 0 || *end != '\0' || *value > maximum)
		return EINVAL;

	/* Succeeded. */
	return 0;
}

/* Makes the controller's folder (0700) when it is not there, and gives its path. */
static int
hidcache_folder(
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

/* Adds a line of one number to a text.  Returns 0 or ENAMETOOLONG. */
static int
hidcache_put(
	char *text,
	size_t size,
	size_t *used,
	const char *format,
	unsigned long value)
{
	int written;

	/* The line after what is there. */
	written = snprintf(text + *used, size - *used, format, value);
	if (written < 0 || (size_t)written >= size - *used)
		return ENAMETOOLONG;
	*used += (size_t)written;
	return 0;
}
