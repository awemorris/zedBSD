/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * bluetoothd's bonds (ws143-p004, see keys.h).
 *
 * A bond's file is text, one key=value a line: type, name (escaped as on
 * the socket), link_key and link_key_type (BR/EDR), ltk, ediv, rand,
 * key_size, authenticated, secure, legacy (LE), irk, identity and
 * identity_type (LE's identity).  Keys are hex of the bytes in the order
 * HCI carries them.  A key the reader does not know is passed over; a
 * known one that is malformed makes the file unreadable.
 */

#include "userland/base/bluetoothd/keys.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The longest bond file. */
#define KEYS_TEXT_MAX		2048U

/* keys_field's answer for a key bluetoothd does not write. */
#define KEYS_FIELD_UNKNOWN	0xffffffffU

static int keys_hex(const char *text, uint8_t *bytes, size_t count);
static void keys_hex_write(char *text, const uint8_t *bytes, size_t count);
static int keys_number(const char *text, unsigned long maximum, unsigned long *value);
static unsigned keys_field(const char *key);
static int keys_line(struct btd_bond *bond, const char *key, const char *value);
static int keys_folder(const char *folder, const uint8_t *controller, char *path, size_t size);

/*
 * Writes the path of a bond's file.  Returns 0, or ENAMETOOLONG.
 */
int
btd_keys_path(
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

	/* The path. */
	written = snprintf(path, size, "%s/%s/%s-%s", folder, own, device, btd_address_type_name(type));
	if (written < 0 || (size_t)written >= size)
		return ENAMETOOLONG;

	/* Succeeded. */
	return 0;
}

/*
 * Writes a bond's file whole: a temporary file of 0600 in the same folder,
 * written and synced, renamed over the old one, and the folder synced.
 * The controller's folder is made (0700) when it is not there.  Returns 0
 * or an errno value.
 */
int
btd_keys_write(
	const char *folder,
	const uint8_t *controller,
	const struct btd_bond *bond)
{
	char path[BTD_KEYS_PATH_MAX];
	char temporary[BTD_KEYS_PATH_MAX + 8U];
	char own_folder[BTD_KEYS_PATH_MAX];
	char text[KEYS_TEXT_MAX];
	ssize_t written;
	size_t length;
	int descriptor;
	int written_name;
	int error;

	/* The text. */
	error = btd_keys_format(bond, text, sizeof(text));
	if (error != 0)
		return error;
	length = strlen(text);

	/* The controller's folder, and the paths. */
	error = keys_folder(folder, controller, own_folder, sizeof(own_folder));
	if (error != 0)
		return error;
	error = btd_keys_path(folder, controller, bond->address, bond->type, path, sizeof(path));
	if (error != 0)
		return error;
	written_name = snprintf(temporary, sizeof(temporary), "%s.tmp", path);
	if (written_name < 0 || (size_t)written_name >= sizeof(temporary))
		return ENAMETOOLONG;

	/* The temporary file (never a link), written and synced. */
	descriptor = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (descriptor < 0)
		return errno;
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

	/* In place of the old one, and the folder synced. */
	error = rename(temporary, path);
	if (error != 0)
		return errno;
	descriptor = open(own_folder, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (descriptor >= 0) {
		(void)fsync(descriptor);
		(void)close(descriptor);
	}

	/* Succeeded: the bond is on disk. */
	return 0;
}

/*
 * Reads a bond's file.  Returns 0, ENOENT when there is none, EBADMSG for a
 * malformed one, or another errno value.
 */
int
btd_keys_read(
	const char *folder,
	const uint8_t *controller,
	const uint8_t *address,
	unsigned type,
	struct btd_bond *bond)
{
	char path[BTD_KEYS_PATH_MAX];
	char text[KEYS_TEXT_MAX];
	ssize_t got;
	int descriptor;
	int error;

	/* The file. */
	error = btd_keys_path(folder, controller, address, type, path, sizeof(path));
	if (error != 0)
		return error;
	descriptor = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (descriptor < 0)
		return errno;
	got = read(descriptor, text, sizeof(text));
	(void)close(descriptor);
	if (got < 0)
		return EIO;

	/* A file as long as the buffer may be cut: refused. */
	if ((size_t)got >= sizeof(text))
		return EBADMSG;

	/* Its fields, which must name the same device. */
	error = btd_keys_parse(text, (size_t)got, bond);
	if (error != 0)
		return error;
	memcpy(bond->address, address, BTD_ADDRESS_BYTES);
	if (bond->type != type)
		return EBADMSG;

	/* Succeeded: the bond. */
	return 0;
}

/*
 * Removes a bond's file.  Returns 0, ENOENT, or another errno value.
 */
int
btd_keys_forget(
	const char *folder,
	const uint8_t *controller,
	const uint8_t *address,
	unsigned type)
{
	char path[BTD_KEYS_PATH_MAX];
	int error;

	/* The file's path (made from the addresses, so nothing outside the folder). */
	error = btd_keys_path(folder, controller, address, type, path, sizeof(path));
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
 * Reads the bonds of a controller, at most max of them, in the order the
 * folder gives (a file whose name or fields are not a bond's is passed
 * over).  Returns 0 with the count (none when the folder is not there), or
 * an errno value.
 */
int
btd_keys_list(
	const char *folder,
	const uint8_t *controller,
	struct btd_bond *bonds,
	unsigned max,
	unsigned *count)
{
	struct dirent *entry;
	char own[24];
	char path[BTD_KEYS_PATH_MAX];
	char address_text[18];
	uint8_t address[BTD_ADDRESS_BYTES];
	const char *dash;
	unsigned type;
	DIR *directory;
	int written;
	int error;

	/* The controller's folder; none is no bond. */
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

	/* Each file named ADDRESS-TYPE (temporary files start with a dot and are not). */
	for (;;) {
		entry = readdir(directory);
		if (entry == NULL)
			break;
		if (*count >= max)
			break;

		/* The address, 17 characters before the dash. */
		dash = strchr(entry->d_name, '-');
		if (entry->d_name[0] == '.' || dash == NULL || dash - entry->d_name != 17)
			continue;
		memcpy(address_text, entry->d_name, 17U);
		address_text[17] = '\0';
		error = btd_address_parse(address_text, address);
		if (error != 0)
			continue;

		/* The type after it. */
		error = btd_address_type_parse(dash + 1, &type);
		if (error != 0)
			continue;

		/* The bond, read and checked like any other. */
		error = btd_keys_read(folder, controller, address, type, &bonds[*count]);
		if (error != 0)
			continue;
		*count += 1U;
	}

	/* The folder is read. */
	(void)closedir(directory);

	/* Succeeded: the bonds found. */
	return 0;
}

/*
 * Writes a bond as its file's text.  Returns 0, or ENAMETOOLONG.
 */
int
btd_keys_format(
	const struct btd_bond *bond,
	char *text,
	size_t size)
{
	char name[4U * BTD_NAME_MAX];
	char hex[33];
	char address[24];
	size_t used;
	int written;
	int error;

	/* The type and the name. */
	error = btd_escape(bond->name, name, sizeof(name));
	if (error != 0)
		name[0] = '\0';
	written = snprintf(text, size, "type=%s\nname=%s\n", btd_address_type_name(bond->type), name);
	if (written < 0 || (size_t)written >= size)
		return ENAMETOOLONG;
	used = (size_t)written;

	/* BR/EDR's link key. */
	if (bond->have_link_key) {
		keys_hex_write(hex, bond->link_key, 16U);
		written = snprintf(text + used, size - used, "link_key=%s\nlink_key_type=%u\n", hex, (unsigned)bond->link_key_type);
		if (written < 0 || (size_t)written >= size - used)
			return ENAMETOOLONG;
		used += (size_t)written;
	}

	/* LE's LTK. */
	if (bond->have_ltk) {
		keys_hex_write(hex, bond->ltk, 16U);
		written = snprintf(text + used, size - used, "ltk=%s\nediv=%u\n", hex, (unsigned)bond->ediv);
		if (written < 0 || (size_t)written >= size - used)
			return ENAMETOOLONG;
		used += (size_t)written;
		keys_hex_write(hex, bond->rand, 8U);
		written = snprintf(text + used, size - used, "rand=%s\nkey_size=%u\n", hex, (unsigned)bond->key_size);
		if (written < 0 || (size_t)written >= size - used)
			return ENAMETOOLONG;
		used += (size_t)written;
	}

	/* How the keys were made. */
	written = snprintf(text + used, size - used, "authenticated=%d\nsecure=%d\nlegacy=%d\n", bond->authenticated != 0, bond->secure != 0, bond->legacy != 0);
	if (written < 0 || (size_t)written >= size - used)
		return ENAMETOOLONG;
	used += (size_t)written;

	/* LE's identity. */
	if (bond->have_irk) {
		keys_hex_write(hex, bond->irk, 16U);
		written = snprintf(text + used, size - used, "irk=%s\n", hex);
		if (written < 0 || (size_t)written >= size - used)
			return ENAMETOOLONG;
		used += (size_t)written;
	}

	/* The identity the device gave, if any. */
	if (bond->have_identity) {
		btd_format_address(bond->identity, address, sizeof(address));
		written = snprintf(text + used, size - used, "identity=%s\nidentity_type=%u\n", address, (unsigned)bond->identity_type);
		if (written < 0 || (size_t)written >= size - used)
			return ENAMETOOLONG;
	}

	/* Succeeded: the text. */
	return 0;
}

/*
 * Reads a bond file's text.  Returns 0, or EBADMSG for a line without '=',
 * a known key malformed, or no type.
 */
int
btd_keys_parse(
	const char *text,
	size_t length,
	struct btd_bond *bond)
{
	char line[600];
	const char *end;
	const char *start;
	char *equals;
	size_t line_length;
	unsigned field;
	int have_type;
	int error;

	/* Nothing read yet. */
	memset(bond, 0, sizeof(*bond));
	have_type = 0;

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

		/* The field. */
		error = keys_line(bond, line, equals + 1);
		if (error != 0)
			return error;
		field = keys_field(line);
		if (field == 0U)
			have_type = 1;
	}

	/* A bond names its type. */
	if (!have_type)
		return EBADMSG;

	/* Succeeded: the bond. */
	return 0;
}

/*
 * Reads an address written as AA:BB:CC:DD:EE:FF into the bytes HCI carries
 * (least significant first).  Returns 0, or EINVAL.
 */
int
btd_address_parse(
	const char *text,
	uint8_t *address)
{
	unsigned index;
	unsigned value;
	size_t length;
	int high;
	int low;
	int read;

	/* Exactly 17 characters. */
	length = strlen(text);
	if (length != 17U)
		return EINVAL;

	/* Six pairs of hex digits, with colons between. */
	for (index = 0U; index < 6U; index++) {
		if (index != 0U && text[index * 3U - 1U] != ':')
			return EINVAL;
		high = text[index * 3U];
		low = text[index * 3U + 1U];
		read = sscanf(text + index * 3U, "%2x", &value);
		if (read != 1)
			return EINVAL;
		if (!((high >= '0' && high <= '9') || (high >= 'a' && high <= 'f') || (high >= 'A' && high <= 'F')))
			return EINVAL;
		if (!((low >= '0' && low <= '9') || (low >= 'a' && low <= 'f') || (low >= 'A' && low <= 'F')))
			return EINVAL;
		address[5U - index] = (uint8_t)value;
	}

	/* Succeeded. */
	return 0;
}

/*
 * Reads an address type's name (bredr, le-public, le-random).  Returns 0, or
 * EINVAL.
 */
int
btd_address_type_parse(
	const char *text,
	unsigned *type)
{
	int same;

	/* Each name. */
	same = strcmp(text, "bredr");
	if (same == 0) {
		*type = BTD_ADDRESS_BREDR;
		return 0;
	}

	/* LE with a public address. */
	same = strcmp(text, "le-public");
	if (same == 0) {
		*type = BTD_ADDRESS_LE_PUBLIC;
		return 0;
	}

	/* LE with a random one. */
	same = strcmp(text, "le-random");
	if (same == 0) {
		*type = BTD_ADDRESS_LE_RANDOM;
		return 0;
	}

	/* Not a type. */
	return EINVAL;
}

/* Reads exactly count bytes of hex digits. */
static int
keys_hex(
	const char *text,
	uint8_t *bytes,
	size_t count)
{
	size_t length;
	size_t index;
	int digit;
	int value;

	/* Exactly 2 x count digits. */
	length = strlen(text);
	if (length != 2U * count)
		return EBADMSG;

	/* Each digit. */
	for (index = 0U; index < 2U * count; index++) {
		digit = text[index];
		if (digit >= '0' && digit <= '9') {
			value = digit - '0';
		} else if (digit >= 'a' && digit <= 'f') {
			value = digit - 'a' + 10;
		} else if (digit >= 'A' && digit <= 'F') {
			value = digit - 'A' + 10;
		} else {
			return EBADMSG;
		}

		/* The high half first. */
		if ((index & 1U) == 0U)
			bytes[index / 2U] = (uint8_t)(value << 4);
		else
			bytes[index / 2U] |= (uint8_t)value;
	}

	/* Succeeded. */
	return 0;
}

/* Writes bytes as hex digits. */
static void
keys_hex_write(
	char *text,
	const uint8_t *bytes,
	size_t count)
{
	size_t index;

	/* Two digits a byte. */
	for (index = 0U; index < count; index++)
		(void)snprintf(text + 2U * index, 3U, "%02x", bytes[index]);
}

/* Reads a decimal number up to a maximum. */
static int
keys_number(
	const char *text,
	unsigned long maximum,
	unsigned long *value)
{
	char *end;

	/* Digits only, in range. */
	if (text[0] < '0' || text[0] > '9')
		return EBADMSG;
	errno = 0;
	*value = strtoul(text, &end, 10);
	if (errno != 0 || *end != '\0' || *value > maximum)
		return EBADMSG;

	/* Succeeded. */
	return 0;
}

/* The keys of a bond's file, in keys_field's order. */
static const char *const keys_names[] = {
	"type", "name", "link_key", "link_key_type", "ltk", "ediv", "rand", "key_size",
	"authenticated", "secure", "legacy", "irk", "identity", "identity_type",
};

/* Finds a key's place in keys_names (KEYS_FIELD_UNKNOWN when it is not there). */
static unsigned
keys_field(
	const char *key)
{
	unsigned index;
	int same;

	/* Each name. */
	for (index = 0U; index < sizeof(keys_names) / sizeof(keys_names[0]); index++) {
		same = strcmp(key, keys_names[index]);
		if (same == 0)
			return index;
	}

	/* Not a key bluetoothd writes. */
	return KEYS_FIELD_UNKNOWN;
}

/* Takes one key=value of a bond's file. */
static int
keys_line(
	struct btd_bond *bond,
	const char *key,
	const char *value)
{
	unsigned long number;
	unsigned field;
	int error;

	/* Each key bluetoothd writes; the others are passed over. */
	error = 0;
	number = 0UL;
	field = keys_field(key);
	switch (field) {
	case 0U:
		error = btd_address_type_parse(value, &bond->type);
		break;
	case 1U:
		(void)snprintf(bond->name, sizeof(bond->name), "%s", value);
		break;
	case 2U:
		error = keys_hex(value, bond->link_key, 16U);
		bond->have_link_key = (error == 0);
		break;
	case 3U:
		error = keys_number(value, 255UL, &number);
		bond->link_key_type = (uint8_t)number;
		break;
	case 4U:
		error = keys_hex(value, bond->ltk, 16U);
		bond->have_ltk = (error == 0);
		break;
	case 5U:
		error = keys_number(value, 65535UL, &number);
		bond->ediv = (uint16_t)number;
		break;
	case 6U:
		error = keys_hex(value, bond->rand, 8U);
		break;
	case 7U:
		error = keys_number(value, 16UL, &number);
		bond->key_size = (uint8_t)number;
		break;
	case 8U:
		error = keys_number(value, 1UL, &number);
		bond->authenticated = (int)number;
		break;
	case 9U:
		error = keys_number(value, 1UL, &number);
		bond->secure = (int)number;
		break;
	case 10U:
		error = keys_number(value, 1UL, &number);
		bond->legacy = (int)number;
		break;
	case 11U:
		error = keys_hex(value, bond->irk, 16U);
		bond->have_irk = (error == 0);
		break;
	case 12U:
		error = btd_address_parse(value, bond->identity);
		bond->have_identity = (error == 0);
		break;
	case 13U:
		error = keys_number(value, 1UL, &number);
		bond->identity_type = (uint8_t)number;
		break;
	default:
		break;
	}

	/* A known key that is malformed. */
	if (error != 0)
		return EBADMSG;

	/* Succeeded: kept, or not known and passed over. */
	return 0;
}

/* Makes the controller's folder (0700) when it is not there, and gives its path. */
static int
keys_folder(
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
