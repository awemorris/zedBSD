/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The efiboot program: lists and edits the UEFI boot entries.
 *
 * It reads and writes the firmware's variables through /dev/system:
 * Boot#### (one EFI_LOAD_OPTION each), BootOrder, BootNext and
 * BootCurrent.  A new entry points at a loader on a GPT partition with a
 * short-form hard-drive device path, which the firmware expands to the
 * disk that holds the partition.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <uapi/block.h>
#include <uapi/system.h>

#include "../diskpart/table.h"

/*
 * The UEFI values efiboot uses.
 *
 * Values referenced: UEFI Specification 2.10, 3.1.3 (EFI_LOAD_OPTION,
 * LOAD_OPTION_ACTIVE), 3.3 (EFI_GLOBAL_VARIABLE, BootOrder, BootNext,
 * BootCurrent), 8.2 (EFI_VARIABLE_NON_VOLATILE, BOOTSERVICE_ACCESS,
 * RUNTIME_ACCESS), 10.3.1 (the end node), 10.3.5.1 (the hard drive node),
 * 10.3.5.4 (the file path node), 10.3.5.6 and 10.3.5.7 (the firmware file
 * and volume nodes).
 */
#define EFI_GLOBAL_VARIABLE		"8be4df61-93ca-11d2-aa0d-00e098032b8c"
#define EFI_VARIABLE_BOOT_ATTRIBUTES	0x00000007U
#define LOAD_OPTION_ACTIVE		0x00000001U
#define DEVICE_PATH_MEDIA		0x04U
#define DEVICE_PATH_END			0x7fU
#define DEVICE_PATH_END_ENTIRE		0xffU
#define MEDIA_HARD_DRIVE		0x01U
#define MEDIA_FILE_PATH			0x04U
#define MEDIA_FIRMWARE_FILE		0x06U
#define MEDIA_FIRMWARE_VOLUME		0x07U
#define HARD_DRIVE_NODE_SIZE		42U
#define HARD_DRIVE_GPT			0x02U
#define HARD_DRIVE_SIGNATURE_GUID	0x02U
#define HARD_DRIVE_SIGNATURE_MBR	0x01U

/* The most entries BootOrder holds, and the size of one load option efiboot reads. */
#define BOOT_ORDER_MAX			512U
#define LOAD_OPTION_MAX			4096U

/* The longest label and loader path efiboot writes, in characters. */
#define LABEL_MAX			64U
#define LOADER_PATH_MAX			255U

/* The label a new entry gets when none is given. */
#define DEFAULT_LABEL			"zedBSD"

/*
 * One Boot#### entry as efiboot shows it.
 *
 * The text fields are UTF-8 decoded from the load option; path is the
 * device path in the firmware's text form, as far as efiboot reads it.
 */
struct boot_entry {
	unsigned number;
	uint32_t attributes;
	char label[LABEL_MAX * 3U + 1U];
	char path[1024];
};

/*
 * The descriptor of /dev/system, opened once by main() and kept until the
 * program ends.
 */
static int system_descriptor = -1;

static int variable_get(const char *name, void *value, size_t capacity, size_t *size);
static int variable_set(const char *name, const void *value, size_t size);
static int variable_next(char *space, char *name);
static int boot_number(const char *name, unsigned *number);
static int number_parse(const char *text, unsigned *number);
static int order_read(uint16_t *order, size_t *count);
static int order_write(const uint16_t *order, size_t count);
static int entry_numbers(unsigned *numbers, size_t capacity, size_t *count);
static int entry_read(unsigned number, uint8_t *option, size_t capacity, size_t *size);
static int entry_describe(const uint8_t *option, size_t size, struct boot_entry *entry);
static size_t path_text(const uint8_t *path, size_t size, char *text, size_t capacity);
static void units_text(const uint8_t *units, size_t count, char *text, size_t capacity);
static void guid_bytes_text(const uint8_t *bytes, char *text);
static int partition_find(const char *disk, unsigned slot, struct dp_part *found, uint32_t *sector_size);
static int device_read(void *context, uint64_t offset, void *data, size_t size);
static size_t path_build(const struct dp_part *partition, const char *loader, uint8_t *path, size_t capacity);
static size_t option_build(const char *label, const uint8_t *path, size_t path_size, uint8_t *option, size_t capacity);
static int command_list(void);
static int command_create(int argc, char **argv);
static int command_delete(const char *operand);
static int command_order(const char *operand);
static int command_next(const char *operand);
static void usage(void);

/*
 * Runs one efiboot command.
 */
int
main(
	int argc,
	char **argv)
{
	int error;

	/* Shows the usage without a command. */
	if (argc < 2) {
		usage();
		return 2;
	}

	/* Opens the device through which the kernel reaches the firmware's variables. */
	system_descriptor = open("/dev/system", O_RDONLY);
	if (system_descriptor < 0) {
		fprintf(stderr, "efiboot: /dev/system: %s\n", strerror(errno));
		return 1;
	}

	/* Runs the command the first operand names. */
	if (strcmp(argv[1], "list") == 0 && argc == 2) {
		error = command_list();
	} else if (strcmp(argv[1], "create") == 0) {
		error = command_create(argc - 2, argv + 2);
	} else if (strcmp(argv[1], "delete") == 0 && argc == 3) {
		error = command_delete(argv[2]);
	} else if (strcmp(argv[1], "order") == 0 && argc == 3) {
		error = command_order(argv[2]);
	} else if (strcmp(argv[1], "next") == 0 && argc == 3) {
		error = command_next(argv[2]);
	} else {
		usage();
		error = EINVAL;
	}

	/* Reports a failed command by its status. */
	close(system_descriptor);
	if (error != 0)
		return 1;

	/* Succeeded. */
	return 0;
}

/*
 * Reads one global variable into value.
 *
 * ENOENT names a variable that does not exist; ERANGE one larger than
 * capacity.
 */
static int
variable_get(
	const char *name,
	void *value,
	size_t capacity,
	size_t *size)
{
	struct system_firmware_variable variable;
	int status;

	/* Asks the kernel for the variable. */
	memset(&variable, 0, sizeof(variable));
	variable.version = KERN_SYSTEM_FIRMWARE_VARIABLE_VERSION;
	variable.value = (uintptr_t)value;
	variable.value_size = (uint32_t)capacity;
	snprintf(variable.space, sizeof(variable.space), "%s", EFI_GLOBAL_VARIABLE);
	snprintf(variable.name, sizeof(variable.name), "%s", name);
	status = ioctl(system_descriptor, KERN_SYSTEM_FIRMWARE_VARIABLE_GET, &variable);
	if (status < 0)
		return errno;

	/* Succeeded: value holds size bytes of the variable. */
	*size = variable.value_size;
	return 0;
}

/* Writes one global variable, or deletes it when size is zero. */
static int
variable_set(
	const char *name,
	const void *value,
	size_t size)
{
	struct system_firmware_variable variable;
	int status;

	/* Asks the kernel to store the variable, kept across restarts and visible at runtime. */
	memset(&variable, 0, sizeof(variable));
	variable.version = KERN_SYSTEM_FIRMWARE_VARIABLE_VERSION;
	variable.attributes = EFI_VARIABLE_BOOT_ATTRIBUTES;
	variable.value = (uintptr_t)value;
	variable.value_size = (uint32_t)size;
	snprintf(variable.space, sizeof(variable.space), "%s", EFI_GLOBAL_VARIABLE);
	snprintf(variable.name, sizeof(variable.name), "%s", name);
	status = ioctl(system_descriptor, KERN_SYSTEM_FIRMWARE_VARIABLE_SET, &variable);
	if (status < 0)
		return errno;

	/* Succeeded: the firmware stored the variable. */
	return 0;
}

/*
 * Replaces space and name with the variable after them; an empty name
 * starts the walk, and ENOENT ends it.
 */
static int
variable_next(
	char *space,
	char *name)
{
	struct system_firmware_variable variable;
	int status;

	/* Asks the kernel for the next variable. */
	memset(&variable, 0, sizeof(variable));
	variable.version = KERN_SYSTEM_FIRMWARE_VARIABLE_VERSION;
	snprintf(variable.space, sizeof(variable.space), "%s", space);
	snprintf(variable.name, sizeof(variable.name), "%s", name);
	status = ioctl(system_descriptor, KERN_SYSTEM_FIRMWARE_VARIABLE_NEXT, &variable);
	if (status < 0)
		return errno;

	/* Succeeded: the caller holds the next variable's space and name. */
	memcpy(space, variable.space, sizeof(variable.space));
	memcpy(name, variable.name, sizeof(variable.name));
	return 0;
}

/* Reads the number of a Boot#### name, or reports EINVAL for another name. */
static int
boot_number(
	const char *name,
	unsigned *number)
{
	int error;

	/* Refuses a name that is not Boot followed by four digits. */
	if (strncmp(name, "Boot", 4) != 0 || strlen(name) != 8U)
		return EINVAL;

	/* Reads the four hexadecimal digits. */
	error = number_parse(name + 4, number);
	if (error != 0)
		return error;

	/* Succeeded: the name is a boot entry's. */
	return 0;
}

/* Reads a boot entry number of exactly four hexadecimal digits. */
static int
number_parse(
	const char *text,
	unsigned *number)
{
	unsigned value;
	unsigned index;
	char digit;

	/* Reads the digits, with uppercase and lowercase letters alike. */
	value = 0;
	for (index = 0; index < 4U; index++) {
		digit = text[index];
		value <<= 4;
		if (digit >= '0' && digit <= '9') {
			value |= (unsigned)(digit - '0');
		} else if (digit >= 'a' && digit <= 'f') {
			value |= (unsigned)(digit - 'a' + 10);
		} else if (digit >= 'A' && digit <= 'F') {
			value |= (unsigned)(digit - 'A' + 10);
		} else {
			/* Refuses a character that is not a digit, including an early end. */
			return EINVAL;
		}
	}

	/* Refuses text that goes on past four digits. */
	if (text[4] != '\0')
		return EINVAL;

	/* Succeeded: the number is the one written. */
	*number = value;
	return 0;
}

/* Reads BootOrder; a firmware without one has an empty order. */
static int
order_read(
	uint16_t *order,
	size_t *count)
{
	uint8_t bytes[BOOT_ORDER_MAX * 2U];
	size_t size;
	size_t index;
	int error;

	/* Reads the variable; its absence is an empty order. */
	error = variable_get("BootOrder", bytes, sizeof(bytes), &size);
	if (error == ENOENT) {
		*count = 0;
		return 0;
	}

	/* Reports why the order could not be read. */
	if (error != 0)
		return error;

	/* Decodes the little-endian numbers. */
	*count = size / 2U;
	for (index = 0; index < *count; index++)
		order[index] = (uint16_t)(bytes[index * 2U] | bytes[index * 2U + 1U] << 8);

	/* Succeeded: order holds count numbers. */
	return 0;
}

/* Writes BootOrder. */
static int
order_write(
	const uint16_t *order,
	size_t count)
{
	uint8_t bytes[BOOT_ORDER_MAX * 2U];
	size_t index;
	int error;

	/* Encodes the numbers little-endian. */
	for (index = 0; index < count; index++) {
		bytes[index * 2U] = (uint8_t)order[index];
		bytes[index * 2U + 1U] = (uint8_t)(order[index] >> 8);
	}

	/* Stores the order. */
	error = variable_set("BootOrder", bytes, count * 2U);
	if (error != 0)
		return error;

	/* Succeeded: the firmware boots in the new order. */
	return 0;
}

/* Collects the numbers of every Boot#### variable, in ascending order. */
static int
entry_numbers(
	unsigned *numbers,
	size_t capacity,
	size_t *count)
{
	char space[KERN_SYSTEM_FIRMWARE_VARIABLE_SPACE_MAX];
	char name[KERN_SYSTEM_FIRMWARE_VARIABLE_NAME_MAX];
	unsigned number;
	size_t index;
	size_t place;
	int error;

	/* Walks every variable from the first. */
	space[0] = '\0';
	name[0] = '\0';
	*count = 0;
	for (;;) {
		error = variable_next(space, name);
		if (error == ENOENT)
			break;

		/* Reports a walk the firmware broke off. */
		if (error != 0)
			return error;

		/* Skips variables that are not global boot entries. */
		if (strcmp(space, EFI_GLOBAL_VARIABLE) != 0)
			continue;
		error = boot_number(name, &number);
		if (error != 0)
			continue;

		/* Stops collecting when the list is full. */
		if (*count == capacity)
			break;

		/* Inserts the number in its ascending place. */
		place = *count;
		while (place > 0 && numbers[place - 1U] > number)
			place--;
		for (index = *count; index > place; index--)
			numbers[index] = numbers[index - 1U];
		numbers[place] = number;
		*count += 1U;
	}

	/* Succeeded: numbers holds count entries. */
	return 0;
}

/* Reads the load option of one boot entry. */
static int
entry_read(
	unsigned number,
	uint8_t *option,
	size_t capacity,
	size_t *size)
{
	char name[16];
	int error;

	/* Reads Boot#### with the number in four uppercase digits. */
	snprintf(name, sizeof(name), "Boot%04X", number);
	error = variable_get(name, option, capacity, size);
	if (error != 0)
		return error;

	/* Succeeded: option holds the load option. */
	return 0;
}

/*
 * Decodes a load option's attributes, label and device path.
 *
 * Values referenced: UEFI Specification 2.10, 3.1.3 (EFI_LOAD_OPTION:
 * Attributes, FilePathListLength, Description, FilePathList).
 */
static int
entry_describe(
	const uint8_t *option,
	size_t size,
	struct boot_entry *entry)
{
	size_t path_size;
	size_t label_end;
	size_t label_units;

	/* Refuses an option too short for its fixed fields. */
	if (size < 6U)
		return EINVAL;

	/* Reads the attributes and the length of the device path. */
	entry->attributes = (uint32_t)option[0] | (uint32_t)option[1] << 8 |
			    (uint32_t)option[2] << 16 | (uint32_t)option[3] << 24;
	path_size = (size_t)(option[4] | option[5] << 8);

	/* Finds the label's terminator. */
	label_end = 6U;
	while (label_end + 1U < size &&
	       (option[label_end] != 0 || option[label_end + 1U] != 0))
		label_end += 2U;

	/* Refuses an option whose label or path runs past its end. */
	if (label_end + 1U >= size || path_size > size - (label_end + 2U))
		return EINVAL;

	/* Decodes the label and the device path. */
	label_units = (label_end - 6U) / 2U;
	units_text(option + 6U, label_units, entry->label, sizeof(entry->label));
	(void)path_text(option + label_end + 2U, path_size, entry->path, sizeof(entry->path));

	/* Succeeded: the entry is described. */
	return 0;
}

/*
 * Writes a device path in the firmware's text form, as far as efiboot
 * knows its nodes; other nodes are shown by their type and subtype.
 * Reports the size of the path up to and with its end node.
 */
static size_t
path_text(
	const uint8_t *path,
	size_t size,
	char *text,
	size_t capacity)
{
	char guid[37];
	char file[LOADER_PATH_MAX * 3U + 1U];
	size_t offset;
	size_t length;
	size_t node_size;
	unsigned type;
	unsigned subtype;
	uint32_t number;
	uint64_t start;
	uint64_t count;

	/* Writes one node at a time, separated by slashes, until the end node. */
	text[0] = '\0';
	offset = 0;
	while (offset + 4U <= size) {
		type = path[offset];
		subtype = path[offset + 1U];
		node_size = (size_t)(path[offset + 2U] | path[offset + 3U] << 8);

		/* Stops at a node that does not fit or the end of the whole path. */
		if (node_size < 4U || node_size > size - offset)
			break;
		if (type == DEVICE_PATH_END && subtype == DEVICE_PATH_END_ENTIRE)
			return offset + node_size;

		/* Separates this node from the one before. */
		length = strlen(text);
		if (length != 0)
			snprintf(text + length, capacity - length, "/");
		length = strlen(text);

		/* Writes the nodes efiboot knows by name, and the others by number. */
		if (type == DEVICE_PATH_MEDIA && subtype == MEDIA_HARD_DRIVE && node_size >= HARD_DRIVE_NODE_SIZE) {
			memcpy(&number, path + offset + 4U, sizeof(number));
			memcpy(&start, path + offset + 8U, sizeof(start));
			memcpy(&count, path + offset + 16U, sizeof(count));
			if (path[offset + 41U] == HARD_DRIVE_SIGNATURE_GUID) {
				guid_bytes_text(path + offset + 24U, guid);
				snprintf(text + length, capacity - length, "HD(%u,GPT,%s,0x%llx,0x%llx)",
					 (unsigned)number, guid,
					 (unsigned long long)start, (unsigned long long)count);
			} else {
				snprintf(text + length, capacity - length, "HD(%u,MBR,0x%02x%02x%02x%02x,0x%llx,0x%llx)",
					 (unsigned)number,
					 path[offset + 27U], path[offset + 26U], path[offset + 25U], path[offset + 24U],
					 (unsigned long long)start, (unsigned long long)count);
			}
		} else if (type == DEVICE_PATH_MEDIA && subtype == MEDIA_FILE_PATH) {
			units_text(path + offset + 4U, (node_size - 4U) / 2U, file, sizeof(file));
			snprintf(text + length, capacity - length, "File(%s)", file);
		} else if (type == DEVICE_PATH_MEDIA && subtype == MEDIA_FIRMWARE_FILE && node_size >= 20U) {
			guid_bytes_text(path + offset + 4U, guid);
			snprintf(text + length, capacity - length, "FvFile(%s)", guid);
		} else if (type == DEVICE_PATH_MEDIA && subtype == MEDIA_FIRMWARE_VOLUME && node_size >= 20U) {
			guid_bytes_text(path + offset + 4U, guid);
			snprintf(text + length, capacity - length, "Fv(%s)", guid);
		} else {
			snprintf(text + length, capacity - length, "Path(%u,%u)", type, subtype);
		}

		/* Steps to the next node. */
		offset += node_size;
	}

	/* The path ended without its end node. */
	return size;
}

/* Writes count UCS-2 units as UTF-8, ending at a terminator or the count. */
static void
units_text(
	const uint8_t *units,
	size_t count,
	char *text,
	size_t capacity)
{
	size_t index;
	size_t length;
	unsigned unit;

	/* Converts one unit at a time while three bytes and the terminator fit. */
	length = 0;
	for (index = 0; index < count && length + 4U <= capacity; index++) {
		unit = (unsigned)(units[index * 2U] | units[index * 2U + 1U] << 8);
		if (unit == 0)
			break;

		/* Writes the unit in one, two or three bytes; a lone surrogate shows as '?'. */
		if (unit < 0x80U) {
			text[length++] = (char)unit;
		} else if (unit < 0x800U) {
			text[length++] = (char)(0xc0U | unit >> 6);
			text[length++] = (char)(0x80U | (unit & 0x3fU));
		} else if (unit >= 0xd800U && unit <= 0xdfffU) {
			text[length++] = '?';
		} else {
			text[length++] = (char)(0xe0U | unit >> 12);
			text[length++] = (char)(0x80U | (unit >> 6 & 0x3fU));
			text[length++] = (char)(0x80U | (unit & 0x3fU));
		}
	}

	/* Ends the text. */
	text[length] = '\0';
}

/* Writes the text form of a GUID laid out as UEFI keeps it in memory. */
static void
guid_bytes_text(
	const uint8_t *bytes,
	char *text)
{
	/* Writes the three little-endian numbers and the eight bytes in order. */
	snprintf(text, 37U, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
		 bytes[3], bytes[2], bytes[1], bytes[0], bytes[5], bytes[4], bytes[7], bytes[6],
		 bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
}

/* Finds one partition of a whole disk by its slot in the GPT. */
static int
partition_find(
	const char *disk,
	unsigned slot,
	struct dp_part *found,
	uint32_t *sector_size)
{
	struct kern_block_info info;
	struct dp_table table;
	struct dp_io io;
	struct stat status;
	char path[64];
	const char *name;
	unsigned index;
	int descriptor;
	int result;
	int error;

	/* Opens the disk by its device name, with or without /dev/. */
	name = disk;
	if (strncmp(name, "/dev/", 5) == 0)
		name += 5;
	if (name[0] == '\0' || strlen(name) >= KERN_BLOCK_NAME_MAX || strchr(name, '/') != NULL)
		return EINVAL;
	snprintf(path, sizeof(path), "/dev/%s", name);
	descriptor = open(path, O_RDONLY);
	if (descriptor < 0)
		return errno;

	/* Refuses a file that is not a block device. */
	error = 0;
	result = fstat(descriptor, &status);
	if (result < 0) {
		error = errno;
	} else if (!S_ISBLK(status.st_mode)) {
		error = EINVAL;
	}

	/* Reads the disk's geometry. */
	memset(&info, 0, sizeof(info));
	info.version = KERN_BLOCK_VERSION;
	info.struct_size = sizeof(info);
	if (error == 0) {
		result = ioctl(descriptor, BLKGETINFO, &info);
		if (result < 0)
			error = errno;
	}

	/* Refuses a partition, which is not a whole disk. */
	if (error == 0 && (info.parent_device != 0 || (info.flags & KERN_BLOCK_PARTITION) != 0))
		error = EINVAL;

	/* Reads the partition table. */
	memset(&table, 0, sizeof(table));
	memset(&io, 0, sizeof(io));
	io.context = &descriptor;
	io.sectors = info.sector_count;
	io.sector_size = info.sector_size;
	io.read = device_read;
	if (error == 0)
		error = dp_load(&table, &io);
	close(descriptor);
	if (error != 0)
		return error;

	/* Refuses a disk without a GPT, where the entry could not name the partition. */
	if (table.format != DP_GPT) {
		dp_free(&table);
		return EINVAL;
	}

	/* Finds the partition in the slot. */
	error = ENOENT;
	for (index = 0; index < table.count; index++) {
		if (table.parts[index].slot == slot) {
			*found = table.parts[index];
			*sector_size = info.sector_size;
			error = 0;
			break;
		}
	}
	dp_free(&table);
	if (error != 0)
		return error;

	/* Succeeded: found describes the partition. */
	return 0;
}

/* Reads size bytes of the disk at a byte offset. */
static int
device_read(
	void *context,
	uint64_t offset,
	void *data,
	size_t size)
{
	ssize_t count;
	int descriptor;

	/* Reads the whole extent, retrying an interrupted read. */
	descriptor = *(int *)context;
	do {
		count = pread(descriptor, data, size, (off_t)offset);
	} while (count < 0 && errno == EINTR);

	/* Reports a failed read. */
	if (count < 0)
		return errno;

	/* Reports a short read as an I/O error. */
	if ((size_t)count != size)
		return EIO;

	/* Succeeded: data holds the extent. */
	return 0;
}

/*
 * Builds the device path HD(...)/File(loader) and its end node.
 *
 * The hard drive node names the GPT partition by its number, its extent in
 * the disk's sectors and its unique GUID.  Reports the path's size, or 0
 * when it does not fit or the loader path is too long.
 *
 * Values referenced: UEFI Specification 2.10, 10.3.5.1 (the hard drive
 * media device path: PartitionNumber, PartitionStart, PartitionSize,
 * PartitionSignature, PartitionFormat, SignatureType), 10.3.5.4 (the file
 * path media device path).
 */
static size_t
path_build(
	const struct dp_part *partition,
	const char *loader,
	uint8_t *path,
	size_t capacity)
{
	uint8_t signature[16];
	uint32_t number;
	size_t loader_length;
	size_t file_size;
	size_t offset;
	size_t index;
	int error;

	/* Refuses a loader path the node cannot hold, or a partition GUID that does not parse. */
	loader_length = strlen(loader);
	if (loader_length == 0 || loader_length > LOADER_PATH_MAX)
		return 0;
	error = dp_guid_parse(partition->uuid, signature);
	if (error != 0)
		return 0;

	/* Refuses a buffer too small for the three nodes. */
	file_size = 4U + (loader_length + 1U) * 2U;
	if (HARD_DRIVE_NODE_SIZE + file_size + 4U > capacity)
		return 0;

	/* Writes the hard drive node. */
	number = partition->slot;
	memset(path, 0, HARD_DRIVE_NODE_SIZE);
	path[0] = DEVICE_PATH_MEDIA;
	path[1] = MEDIA_HARD_DRIVE;
	path[2] = HARD_DRIVE_NODE_SIZE;
	memcpy(path + 4U, &number, sizeof(number));
	memcpy(path + 8U, &partition->start, sizeof(partition->start));
	memcpy(path + 16U, &partition->count, sizeof(partition->count));
	memcpy(path + 24U, signature, sizeof(signature));
	path[40] = HARD_DRIVE_GPT;
	path[41] = HARD_DRIVE_SIGNATURE_GUID;
	offset = HARD_DRIVE_NODE_SIZE;

	/* Writes the file path node, with the firmware's backslashes and its terminator. */
	path[offset] = DEVICE_PATH_MEDIA;
	path[offset + 1U] = MEDIA_FILE_PATH;
	path[offset + 2U] = (uint8_t)file_size;
	path[offset + 3U] = (uint8_t)(file_size >> 8);
	for (index = 0; index <= loader_length; index++) {
		path[offset + 4U + index * 2U] = (uint8_t)loader[index];
		if (loader[index] == '/')
			path[offset + 4U + index * 2U] = '\\';
		path[offset + 5U + index * 2U] = 0;
	}
	offset += file_size;

	/* Writes the end node. */
	path[offset] = DEVICE_PATH_END;
	path[offset + 1U] = DEVICE_PATH_END_ENTIRE;
	path[offset + 2U] = 4U;
	path[offset + 3U] = 0;
	offset += 4U;

	/* Succeeded: path holds offset bytes. */
	return offset;
}

/*
 * Builds an active load option: its attributes, the device path's size,
 * the label in UCS-2 with its terminator, and the device path.  Reports
 * its size, or 0 when it does not fit.
 */
static size_t
option_build(
	const char *label,
	const uint8_t *path,
	size_t path_size,
	uint8_t *option,
	size_t capacity)
{
	size_t label_length;
	size_t offset;
	size_t index;

	/* Refuses a label that is too long, or an option that does not fit. */
	label_length = strlen(label);
	if (label_length > LABEL_MAX)
		return 0;
	if (6U + (label_length + 1U) * 2U + path_size > capacity)
		return 0;

	/* Writes the attributes and the device path's size. */
	option[0] = (uint8_t)LOAD_OPTION_ACTIVE;
	option[1] = 0;
	option[2] = 0;
	option[3] = 0;
	option[4] = (uint8_t)path_size;
	option[5] = (uint8_t)(path_size >> 8);

	/* Writes the label, one byte of ASCII to one unit, with its terminator. */
	offset = 6U;
	for (index = 0; index <= label_length; index++) {
		option[offset] = (uint8_t)label[index];
		option[offset + 1U] = 0;
		offset += 2U;
	}

	/* Writes the device path. */
	memcpy(option + offset, path, path_size);
	offset += path_size;

	/* Succeeded: option holds offset bytes. */
	return offset;
}

/* Lists BootCurrent, BootNext, BootOrder and every boot entry. */
static int
command_list(
	void)
{
	static uint8_t option[LOAD_OPTION_MAX];
	static unsigned numbers[BOOT_ORDER_MAX];
	uint16_t order[BOOT_ORDER_MAX];
	struct boot_entry entry;
	uint8_t number[2];
	size_t count;
	size_t size;
	size_t index;
	int error;

	/* Shows the entry the firmware started this boot from, when it says. */
	error = variable_get("BootCurrent", number, sizeof(number), &size);
	if (error == 0 && size == 2U)
		printf("BootCurrent: %04X\n", (unsigned)(number[0] | number[1] << 8));

	/* Reports a firmware whose variables the kernel cannot reach. */
	if (error != 0 && error != ENOENT) {
		fprintf(stderr, "efiboot: BootCurrent: %s\n", strerror(error));
		return error;
	}

	/* Shows the entry the next boot takes once, when one is set. */
	error = variable_get("BootNext", number, sizeof(number), &size);
	if (error == 0 && size == 2U)
		printf("BootNext: %04X\n", (unsigned)(number[0] | number[1] << 8));

	/* Shows the boot order. */
	error = order_read(order, &count);
	if (error != 0) {
		fprintf(stderr, "efiboot: BootOrder: %s\n", strerror(error));
		return error;
	}
	printf("BootOrder:");
	for (index = 0; index < count; index++)
		printf("%s%04X", index == 0 ? " " : ",", order[index]);
	printf("\n");

	/* Collects the entries. */
	error = entry_numbers(numbers, BOOT_ORDER_MAX, &count);
	if (error != 0) {
		fprintf(stderr, "efiboot: variables: %s\n", strerror(error));
		return error;
	}

	/* Shows each entry with its label and device path; '*' marks an active one. */
	for (index = 0; index < count; index++) {
		error = entry_read(numbers[index], option, sizeof(option), &size);
		if (error == 0)
			error = entry_describe(option, size, &entry);
		if (error != 0) {
			printf("Boot%04X  (%s)\n", numbers[index], strerror(error));
			continue;
		}

		/* Writes the entry's line. */
		printf("Boot%04X%c %s\t%s\n",
		       numbers[index],
		       (entry.attributes & LOAD_OPTION_ACTIVE) != 0 ? '*' : ' ',
		       entry.label,
		       entry.path);
	}

	/* Succeeded. */
	return 0;
}

/*
 * Creates, or finds, the boot entry for a loader on a GPT partition and
 * puts it at the end of BootOrder.
 *
 * An entry with the same device path is used again instead of making a
 * second one.  -l names the label (zedBSD by default) and -n makes the
 * next boot take the entry once.
 */
static int
command_create(
	int argc,
	char **argv)
{
	static uint8_t option[LOAD_OPTION_MAX];
	static uint8_t existing[LOAD_OPTION_MAX];
	static unsigned numbers[BOOT_ORDER_MAX];
	uint16_t order[BOOT_ORDER_MAX];
	uint8_t path[HARD_DRIVE_NODE_SIZE + 4U + (LOADER_PATH_MAX + 1U) * 2U + 4U];
	struct dp_part partition;
	struct boot_entry entry;
	const char *label;
	char name[16];
	uint8_t next[2];
	uint32_t sector_size;
	unsigned slot;
	unsigned number;
	size_t path_size;
	size_t option_size;
	size_t existing_size;
	size_t count;
	size_t order_count;
	size_t index;
	int set_next;
	int found;
	int same;
	int error;
	char *end;

	/* Reads the options before the three operands. */
	label = DEFAULT_LABEL;
	set_next = 0;
	while (argc > 0 && argv[0][0] == '-') {
		if (strcmp(argv[0], "-l") == 0 && argc > 1) {
			label = argv[1];
			argc -= 2;
			argv += 2;
		} else if (strcmp(argv[0], "-n") == 0) {
			set_next = 1;
			argc -= 1;
			argv += 1;
		} else {
			usage();
			return EINVAL;
		}
	}

	/* Refuses anything but a disk, a partition number and a loader path. */
	if (argc != 3) {
		usage();
		return EINVAL;
	}
	errno = 0;
	slot = (unsigned)strtoul(argv[1], &end, 10);
	if (errno != 0 || *end != '\0' || slot == 0) {
		fprintf(stderr, "efiboot: %s: not a partition number\n", argv[1]);
		return EINVAL;
	}

	/* Finds the partition on the disk. */
	error = partition_find(argv[0], slot, &partition, &sector_size);
	if (error != 0) {
		fprintf(stderr, "efiboot: %s partition %u: %s\n", argv[0], slot, strerror(error));
		return error;
	}

	/* Builds the load option. */
	path_size = path_build(&partition, argv[2], path, sizeof(path));
	option_size = 0;
	if (path_size != 0)
		option_size = option_build(label, path, path_size, option, sizeof(option));
	if (option_size == 0) {
		fprintf(stderr, "efiboot: the label or the loader path is too long\n");
		return EINVAL;
	}

	/* Collects the existing entries. */
	error = entry_numbers(numbers, BOOT_ORDER_MAX, &count);
	if (error != 0) {
		fprintf(stderr, "efiboot: variables: %s\n", strerror(error));
		return error;
	}

	/* Looks for an entry with the same device path, and otherwise the lowest free number. */
	found = 0;
	number = 0;
	for (index = 0; index < count; index++) {
		/* Moves past the numbers taken so far; the list is ascending. */
		if (numbers[index] == number)
			number++;

		/* Reads the entry; one that cannot be read is not the same. */
		error = entry_read(numbers[index], existing, sizeof(existing), &existing_size);
		if (error != 0)
			continue;
		error = entry_describe(existing, existing_size, &entry);
		if (error != 0)
			continue;

		/* Uses again an entry whose load option ends with the new device path. */
		same = 0;
		if (existing_size >= path_size)
			same = memcmp(existing + existing_size - path_size, path, path_size) == 0;
		if (same) {
			number = numbers[index];
			found = 1;
			break;
		}
	}

	/* Refuses a firmware whose every entry number is taken. */
	if (!found && number > 0xffffU) {
		fprintf(stderr, "efiboot: no boot entry number is free\n");
		return ENOSPC;
	}

	/* Writes the new entry. */
	if (!found) {
		snprintf(name, sizeof(name), "Boot%04X", number);
		error = variable_set(name, option, option_size);
		if (error != 0) {
			fprintf(stderr, "efiboot: %s: %s\n", name, strerror(error));
			return error;
		}
	}

	/* Puts the entry at the end of BootOrder when it is not in it yet. */
	error = order_read(order, &order_count);
	if (error != 0) {
		fprintf(stderr, "efiboot: BootOrder: %s\n", strerror(error));
		return error;
	}
	for (index = 0; index < order_count; index++) {
		if (order[index] == number)
			break;
	}
	if (index == order_count) {
		/* Refuses an order that has no room left. */
		if (order_count == BOOT_ORDER_MAX) {
			fprintf(stderr, "efiboot: BootOrder is full\n");
			return ENOSPC;
		}

		/* Appends the entry and stores the order. */
		order[order_count] = (uint16_t)number;
		error = order_write(order, order_count + 1U);
		if (error != 0) {
			fprintf(stderr, "efiboot: BootOrder: %s\n", strerror(error));
			return error;
		}
	}

	/* Makes the next boot take the entry, when asked. */
	if (set_next) {
		next[0] = (uint8_t)number;
		next[1] = (uint8_t)(number >> 8);
		error = variable_set("BootNext", next, sizeof(next));
		if (error != 0) {
			fprintf(stderr, "efiboot: BootNext: %s\n", strerror(error));
			return error;
		}
	}

	/* Succeeded: tells the caller the entry's number. */
	printf("Boot%04X\n", number);
	return 0;
}

/* Deletes one boot entry and takes it out of BootOrder. */
static int
command_delete(
	const char *operand)
{
	uint16_t order[BOOT_ORDER_MAX];
	char name[16];
	unsigned number;
	size_t count;
	size_t index;
	size_t kept;
	int error;

	/* Reads the entry's number. */
	error = number_parse(operand, &number);
	if (error != 0) {
		fprintf(stderr, "efiboot: %s: not a boot entry number\n", operand);
		return error;
	}

	/* Deletes the entry. */
	snprintf(name, sizeof(name), "Boot%04X", number);
	error = variable_set(name, NULL, 0);
	if (error != 0) {
		fprintf(stderr, "efiboot: %s: %s\n", name, strerror(error));
		return error;
	}

	/* Takes the entry out of the order, when it is there. */
	error = order_read(order, &count);
	if (error != 0) {
		fprintf(stderr, "efiboot: BootOrder: %s\n", strerror(error));
		return error;
	}
	kept = 0;
	for (index = 0; index < count; index++) {
		if (order[index] != number)
			order[kept++] = order[index];
	}
	if (kept != count) {
		error = order_write(order, kept);
		if (error != 0) {
			fprintf(stderr, "efiboot: BootOrder: %s\n", strerror(error));
			return error;
		}
	}

	/* Succeeded. */
	return 0;
}

/* Replaces BootOrder with a comma-separated list of entry numbers. */
static int
command_order(
	const char *operand)
{
	uint16_t order[BOOT_ORDER_MAX];
	char digits[5];
	const char *cursor;
	unsigned number;
	size_t count;
	int error;

	/* Reads each four-digit number before a comma or the end. */
	count = 0;
	cursor = operand;
	for (;;) {
		/* Refuses a number that is not four digits followed by a comma or the end. */
		if (strlen(cursor) < 4U || (cursor[4] != ',' && cursor[4] != '\0')) {
			fprintf(stderr, "efiboot: %s: not a list of boot entry numbers\n", operand);
			return EINVAL;
		}
		memcpy(digits, cursor, 4U);
		digits[4] = '\0';
		error = number_parse(digits, &number);
		if (error != 0 || count == BOOT_ORDER_MAX) {
			fprintf(stderr, "efiboot: %s: not a list of boot entry numbers\n", operand);
			return EINVAL;
		}
		order[count++] = (uint16_t)number;

		/* Ends after the last number. */
		if (cursor[4] == '\0')
			break;
		cursor += 5;
	}

	/* Stores the order. */
	error = order_write(order, count);
	if (error != 0) {
		fprintf(stderr, "efiboot: BootOrder: %s\n", strerror(error));
		return error;
	}

	/* Succeeded. */
	return 0;
}

/* Makes the next boot take one entry, once. */
static int
command_next(
	const char *operand)
{
	uint8_t next[2];
	unsigned number;
	int error;

	/* Reads the entry's number. */
	error = number_parse(operand, &number);
	if (error != 0) {
		fprintf(stderr, "efiboot: %s: not a boot entry number\n", operand);
		return error;
	}

	/* Stores BootNext. */
	next[0] = (uint8_t)number;
	next[1] = (uint8_t)(number >> 8);
	error = variable_set("BootNext", next, sizeof(next));
	if (error != 0) {
		fprintf(stderr, "efiboot: BootNext: %s\n", strerror(error));
		return error;
	}

	/* Succeeded. */
	return 0;
}

/* Shows how efiboot is used. */
static void
usage(
	void)
{
	/* Writes one line for each command. */
	fprintf(stderr,
		"usage: efiboot list\n"
		"       efiboot create [-l label] [-n] disk partition loader\n"
		"       efiboot delete XXXX\n"
		"       efiboot order XXXX[,XXXX...]\n"
		"       efiboot next XXXX\n");
}
