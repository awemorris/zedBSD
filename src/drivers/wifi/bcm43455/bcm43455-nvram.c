/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * BCM43455: checking the package's files, converting the NVRAM text and
 * cutting the CLM into clmload messages.
 *
 * Pure functions: no lock, no allocation, no hardware.
 *
 */

#include "drivers/wifi/bcm43455/bcm43455-firmware.h"
#include "drivers/wifi/bcm43455/bcm43455-bytes.h"
#include <kern/kcrt.h>
#include <uapi/errno.h>

/* The text that starts the firmware's version, naming the chip and its ROM. */
#define VERSION_MARK			"43455c0-roml/"
#define VERSION_MARK_LENGTH		13U

/* The CLM container's first four bytes. */
#define CLM_MAGIC			"BLOB"
#define CLM_MAGIC_LENGTH		4U

/*
 * The clmload header: the flags, the kind of download, the length and a
 * checksum.
 *
 * Values referenced: Infineon wifi-host-driver bd99da7b
 * WHD/COMPONENT_WIFI6/src/include/whd_wlioctl.h (lines 4154-4172) and OpenBSD
 * sys/dev/ic/bwfmreg.h revision 1.26 (lines 785-797).
 */
#define CLM_FLAG			0U
#define CLM_TYPE			2U
#define CLM_LENGTH			4U
#define CLM_CRC				8U

/*
 * The clmload flags: the handler's version, and the first and the last piece.
 *
 * Values referenced: Infineon wifi-host-driver bd99da7b
 * WHD/COMPONENT_WIFI6/src/include/whd_wlioctl.h (lines 4154-4172) and OpenBSD
 * sys/dev/ic/bwfmreg.h revision 1.26 (lines 785-797).
 */
#define CLM_FLAG_HANDLER_VERSION	0x1000U
#define CLM_FLAG_BEGIN			0x0002U
#define CLM_FLAG_END			0x0004U

/*
 * The download kind that means the regulatory table.
 *
 * Value referenced: Infineon wifi-host-driver bd99da7b
 * WHD/COMPONENT_WIFI6/src/include/whd_wlioctl.h (lines 4154-4172) and OpenBSD
 * sys/dev/ic/bwfmreg.h revision 1.26 (lines 785-797).
 */
#define CLM_TYPE_CLM			2U

/*
 * The NVRAM key of the board's placeholder address, and an address's text
 * length.
 */
#define MAC_KEY				"macaddr"
#define MAC_KEY_LENGTH			7U
#define MAC_TEXT_LENGTH			17U

static int line_bounds(const uint8_t *text, size_t length, size_t start, size_t *first, size_t *last, size_t *next);
static int is_setting(const uint8_t *text, size_t first, size_t last);
static int hex_digit(uint8_t character, unsigned *value);
static int is_blank(uint8_t character);

/*
 * Checks the firmware image and copies its version text.
 */
int
bcm43455_firmware_check(
	const uint8_t *firmware,
	size_t length,
	char *version,
	size_t version_capacity)
{
	size_t offset;
	size_t copied;
	int same;

	/* Refuses a missing image, a missing destination, or another size. */
	if (firmware == NULL ||
	    version == NULL ||
	    version_capacity == 0)
		return EINVAL;
	if (length != BCM43455_FIRMWARE_SIZE)
		return EINVAL;

	/* Looks for the version mark anywhere in the image. */
	for (offset = 0; offset + VERSION_MARK_LENGTH <= length; offset++) {
		same = kern_memcmp(firmware + offset, VERSION_MARK, VERSION_MARK_LENGTH);
		if (same == 0)
			break;
	}

	/* An image without the mark is not this chip's firmware. */
	if (offset + VERSION_MARK_LENGTH > length)
		return EINVAL;

	/* Copies the printable text from the mark on, up to its end. */
	copied = 0;
	while (copied + 1U < version_capacity && offset + copied < length) {
		/*
		 * The text ends at its NUL or at the first byte that is not
		 * text.
		 */
		if (firmware[offset + copied] < 0x20U || firmware[offset + copied] > 0x7eU)
			break;
		version[copied] = (char)firmware[offset + copied];
		copied++;
	}

	/* Ends the copied text. */
	version[copied] = '\0';

	/* Succeeded: the image names this chip. */
	return 0;
}

/*
 * Checks the CLM file.
 */
int
bcm43455_clm_check(
	const uint8_t *clm,
	size_t length)
{
	int same;

	/* Refuses a missing file or another size. */
	if (clm == NULL)
		return EINVAL;
	if (length != BCM43455_CLM_SIZE)
		return EINVAL;

	/* The container starts with its magic. */
	same = kern_memcmp(clm, CLM_MAGIC, CLM_MAGIC_LENGTH);
	if (same != 0)
		return EINVAL;

	/* Succeeded: the file is a CLM container. */
	return 0;
}

/*
 * Checks the NVRAM file.
 */
int
bcm43455_nvram_check(
	const uint8_t *text,
	size_t length)
{
	size_t i;

	/* Refuses a missing file or another size. */
	if (text == NULL)
		return EINVAL;
	if (length != BCM43455_NVRAM_SIZE)
		return EINVAL;

	/* Every byte is printable, a tab, or a line end. */
	for (i = 0; i < length; i++) {
		if (text[i] == '\t' ||
		    text[i] == '\n' ||
		    text[i] == '\r')
			continue;
		if (text[i] < 0x20U || text[i] > 0x7eU)
			return EINVAL;
	}

	/* Succeeded: the file is text. */
	return 0;
}

/*
 * Turns the NVRAM text into the chip's form: the settings separated by NULs,
 * padded to four bytes, then the length word.  Spaces around a line and lines
 * without "=" are dropped as well.
 *
 * Procedure referenced: OpenBSD sys/dev/ic/bwfm.c revision 1.113
 * (bwfm_nvram_convert(), lines 3009 on).
 */
int
bcm43455_nvram_pack(
	const uint8_t *text,
	size_t length,
	uint8_t *blob,
	size_t capacity,
	size_t *blob_length)
{
	size_t start;
	size_t first;
	size_t last;
	size_t next;
	size_t used;
	size_t settings;
	size_t line_length;
	uint32_t words;
	uint32_t token;
	int found;
	int setting;

	/* Refuses missing buffers. */
	if (text == NULL ||
	    blob == NULL ||
	    blob_length == NULL)
		return EINVAL;

	/* Copies every setting line, each with its NUL. */
	used = 0;
	settings = 0;
	start = 0;
	for (;;) {
		/* Finds the next line's text without its surrounding spaces. */
		found = line_bounds(text, length, start, &first, &last, &next);
		if (!found)
			break;
		start = next;

		/* Keeps only lines of the form key=value. */
		setting = is_setting(text, first, last);
		if (!setting)
			continue;

		/*
		 * Refuses a list that would not fit with its end and its word.
		 */
		line_length = last - first;
		if (line_length + 1U > capacity - used)
			return ENOSPC;

		/* Copies the line and its NUL. */
		kern_memcpy(blob + used, text + first, line_length);
		used += line_length;
		blob[used] = 0;
		used++;
		settings++;
	}

	/* A file without one setting is not this board's NVRAM. */
	if (settings == 0)
		return EINVAL;

	/* Ends the list with a NUL. */
	if (used + 1U > capacity)
		return ENOSPC;
	blob[used] = 0;
	used++;

	/* Pads the list to whole words. */
	while ((used % 4U) != 0) {
		if (used + 1U > capacity)
			return ENOSPC;
		blob[used] = 0;
		used++;
	}

	/*
	 * Appends the length word: the words in the low half, their complement
	 * in the high half.
	 */
	if (used + 4U > capacity)
		return ENOSPC;
	words = (uint32_t)(used / 4U);
	token = (~words << 16) | (words & 0xffffU);
	bcm43455_put_le32(blob + used, token);
	used += 4U;

	/* Succeeded: the blob is ready for the top of RAM. */
	*blob_length = used;
	return 0;
}

/*
 * Reads the NVRAM text's macaddr setting.
 */
int
bcm43455_nvram_mac(
	const uint8_t *text,
	size_t length,
	uint8_t mac[6])
{
	size_t start;
	size_t first;
	size_t last;
	size_t next;
	size_t value;
	unsigned high;
	unsigned low;
	unsigned i;
	int found;
	int same;
	int error;

	/* Refuses missing buffers. */
	if (text == NULL || mac == NULL)
		return EINVAL;

	/* Looks for the line that sets macaddr. */
	start = 0;
	for (;;) {
		/* Finds the next line; the end of the text means no address. */
		found = line_bounds(text, length, start, &first, &last, &next);
		if (!found)
			return ENOENT;
		start = next;

		/* Skips a line too short to be the setting. */
		if (last - first < MAC_KEY_LENGTH + 1U)
			continue;

		/* Stops at the line whose key is macaddr. */
		same = kern_memcmp(text + first, MAC_KEY, MAC_KEY_LENGTH);
		if (same == 0 && text[first + MAC_KEY_LENGTH] == '=')
			break;
	}

	/* The value is six two-digit hexadecimal bytes separated by colons. */
	value = first + MAC_KEY_LENGTH + 1U;
	if (last - value != MAC_TEXT_LENGTH)
		return EINVAL;
	for (i = 0; i < 6U; i++) {
		/* Reads the byte's two digits. */
		error = hex_digit(text[value + i * 3U], &high);
		if (error != 0)
			return error;
		error = hex_digit(text[value + i * 3U + 1U], &low);
		if (error != 0)
			return error;
		mac[i] = (uint8_t)((high << 4) | low);

		/* A colon follows every byte but the last. */
		if (i < 5U && text[value + i * 3U + 2U] != ':')
			return EINVAL;
	}

	/* Succeeded: mac holds the setting. */
	return 0;
}

/*
 * Finds where the converted NVRAM goes: the top of RAM, its last four bytes the
 * length word.
 *
 * Procedure referenced: OpenBSD sys/dev/sdmmc/if_bwfm_sdio.c revision 1.44
 * (lines 526-528).
 */
int
bcm43455_nvram_place(
	uint32_t ram_base,
	uint32_t ram_size,
	size_t firmware_length,
	size_t blob_length,
	uint32_t *address)
{
	uint64_t top;
	uint64_t start;

	/*
	 * Refuses a blob that is not whole words, or nowhere to put the answer.
	 */
	if (address == NULL ||
	    blob_length == 0 ||
	    (blob_length % 4U) != 0)
		return EINVAL;

	/* Refuses a blob and a firmware that do not fit RAM together. */
	if ((uint64_t)blob_length + (uint64_t)firmware_length > (uint64_t)ram_size)
		return EINVAL;

	/*
	 * The blob ends at the top of RAM, so its length word is RAM's last
	 * word.
	 */
	top = (uint64_t)ram_base + (uint64_t)ram_size;
	start = top - (uint64_t)blob_length;

	/* Succeeded: the blob's first byte. */
	*address = (uint32_t)start;
	return 0;
}

/*
 * Builds the clmload message that carries the CLM bytes from offset on.
 *
 * Procedure referenced: Infineon wifi-host-driver bd99da7b whd_clm.c (lines
 * 54-58).
 */
int
bcm43455_clm_message(
	uint8_t *message,
	size_t capacity,
	const uint8_t *clm,
	size_t clm_length,
	size_t offset,
	size_t *message_length,
	size_t *taken)
{
	size_t chunk;
	uint16_t flags;

	/* Refuses missing buffers and an offset with nothing left. */
	if (message == NULL ||
	    clm == NULL ||
	    message_length == NULL ||
	    taken == NULL)
		return EINVAL;
	if (offset >= clm_length)
		return EINVAL;

	/* Takes up to one message's worth of CLM bytes. */
	chunk = clm_length - offset;
	if (chunk > BCM43455_CLM_CHUNK_MAX)
		chunk = BCM43455_CLM_CHUNK_MAX;
	if (capacity < BCM43455_CLM_HEADER_LENGTH + chunk)
		return ENOSPC;

	/* Marks the first and the last piece. */
	flags = CLM_FLAG_HANDLER_VERSION;
	if (offset == 0)
		flags |= CLM_FLAG_BEGIN;
	if (offset + chunk == clm_length)
		flags |= CLM_FLAG_END;

	/* Writes the header; the firmware does not check a zero checksum. */
	bcm43455_put_le16(message + CLM_FLAG, flags);
	bcm43455_put_le16(message + CLM_TYPE, (uint16_t)CLM_TYPE_CLM);
	bcm43455_put_le32(message + CLM_LENGTH, (uint32_t)chunk);
	bcm43455_put_le32(message + CLM_CRC, 0);

	/* The CLM bytes follow the header. */
	kern_memcpy(message + BCM43455_CLM_HEADER_LENGTH, clm + offset, chunk);

	/* Succeeded: the message is ready to send. */
	*message_length = BCM43455_CLM_HEADER_LENGTH + chunk;
	*taken = chunk;
	return 0;
}

/*
 * Finds the line that starts at start, without its surrounding spaces;
 * returns 0 once the text is used up.
 */
static int
line_bounds(
	const uint8_t *text,
	size_t length,
	size_t start,
	size_t *first,
	size_t *last,
	size_t *next)
{
	size_t end;
	int blank;

	/* Nothing is left. */
	if (start >= length)
		return 0;

	/* The line runs to its newline or to the end of the text. */
	end = start;
	while (end < length && text[end] != '\n')
		end++;
	*next = end + 1U;

	/*
	 * Drops the spaces, tabs and carriage returns before the line's text.
	 */
	*first = start;
	while (*first < end) {
		blank = is_blank(text[*first]);
		if (!blank)
			break;
		(*first)++;
	}

	/* And those after it. */
	*last = end;
	while (*last > *first) {
		blank = is_blank(text[*last - 1U]);
		if (!blank)
			break;
		(*last)--;
	}

	/* Succeeded: a line was found, perhaps an empty one. */
	return 1;
}

/*
 * Reports whether a trimmed line is a key=value setting rather than a comment
 * or blank.
 */
static int
is_setting(
	const uint8_t *text,
	size_t first,
	size_t last)
{
	size_t i;

	/* A blank line or a comment is not a setting. */
	if (first == last)
		return 0;
	if (text[first] == '#')
		return 0;

	/* A setting has a key before its equals sign. */
	for (i = first + 1U; i < last; i++) {
		if (text[i] == '=')
			return 1;
	}

	/* A line without an equals sign is not a setting. */
	return 0;
}

/* Reads one hexadecimal digit. */
static int
hex_digit(
	uint8_t character,
	unsigned *value)
{
	/* Decimal digits. */
	if (character >= '0' && character <= '9') {
		*value = (unsigned)(character - '0');
		return 0;
	}

	/* Lower-case letters. */
	if (character >= 'a' && character <= 'f') {
		*value = (unsigned)(character - 'a') + 10U;
		return 0;
	}

	/* Upper-case letters. */
	if (character >= 'A' && character <= 'F') {
		*value = (unsigned)(character - 'A') + 10U;
		return 0;
	}

	/* Anything else is not a digit. */
	return EINVAL;
}

/*
 * Reports whether a byte is a space, a tab or a carriage return around a line's
 * text.
 */
static int
is_blank(
	uint8_t character)
{
	/* The three kinds of blank a line may carry. */
	if (character == ' ')
		return 1;
	if (character == '\t')
		return 1;
	if (character == '\r')
		return 1;

	/* Anything else is text. */
	return 0;
}
