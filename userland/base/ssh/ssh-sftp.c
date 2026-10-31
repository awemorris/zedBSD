/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The SSH file transfer protocol version 3, the server side: the framing
 * of packets, the reading of requests and the writing of replies.
 */

#include <string.h>

#include "ssh-sftp.h"

/*
 * The flag bits an ATTRS may carry.  A bit outside this set is a protocol
 * error (the draft's section 5), because only version 3 is announced.
 */
#define ATTR_FLAGS_KNOWN	(SSH_FILEXFER_ATTR_SIZE | SSH_FILEXFER_ATTR_UIDGID | SSH_FILEXFER_ATTR_PERMISSIONS | SSH_FILEXFER_ATTR_ACMODTIME | SSH_FILEXFER_ATTR_EXTENDED)

/* The flag bits whose fields are written into an ATTRS; no extended pair is ever sent. */
#define ATTR_FLAGS_WRITTEN	(SSH_FILEXFER_ATTR_SIZE | SSH_FILEXFER_ATTR_UIDGID | SSH_FILEXFER_ATTR_PERMISSIONS | SSH_FILEXFER_ATTR_ACMODTIME)

/* The longest handle (draft section 6: a handle string is at most 256 bytes). */
#define HANDLE_MAX		256U

/* The bytes of a packet that precede its type: the length field. */
#define PACKET_LENGTH_SIZE	4U

/*
 * The part of a mode that says what kind of file it is, and the kinds, as
 * POSIX defines the bits of st_mode (the draft's section 5 names POSIX for
 * the permissions field).
 *
 * Values referenced: POSIX.1 <sys/stat.h> S_IFMT and its file type bits,
 * written in octal.
 */
#define MODE_TYPE_MASK		0170000U
#define MODE_TYPE_SOCKET	0140000U
#define MODE_TYPE_SYMLINK	0120000U
#define MODE_TYPE_REGULAR	0100000U
#define MODE_TYPE_BLOCK		0060000U
#define MODE_TYPE_DIRECTORY	0040000U
#define MODE_TYPE_CHARACTER	0020000U
#define MODE_TYPE_FIFO		0010000U

/*
 * The special permission bits of a mode.
 *
 * Values referenced: POSIX.1 <sys/stat.h> S_ISUID, S_ISGID, S_ISVTX.
 */
#define MODE_SETUID		04000U
#define MODE_SETGID		02000U
#define MODE_STICKY		01000U

/*
 * How far in the past a modification time may be and still be shown as a
 * time of day: half of the mean Gregorian year of 31556952 seconds, which
 * is the "six months" of an "ls -l" listing.
 */
#define RECENT_SECONDS		15778476U

/* The seconds in a day, in an hour and in a minute, to split a time into a date. */
#define SECONDS_PER_DAY		86400U
#define SECONDS_PER_HOUR	3600U
#define SECONDS_PER_MINUTE	60U

/*
 * The narrowest each field of the long name is written, the link count
 * and the size aligned right and the owner and group aligned left.
 *
 * Values referenced: draft-ietf-secsh-filexfer-02 section 7 (the longname
 * format: the widths "1234567890 123 12345678 12345678 12345678
 * 123456789012").
 */
#define LONG_NAME_LINKS_WIDTH	3U
#define LONG_NAME_OWNER_WIDTH	8U
#define LONG_NAME_GROUP_WIDTH	8U
#define LONG_NAME_SIZE_WIDTH	8U

/*
 * The English month abbreviations of the date field of the long name.
 *
 * Values referenced: draft-ietf-secsh-filexfer-02 section 7 (the longname
 * example "Mar 25 14:29"; the other eleven are the same three-letter
 * English forms).
 */
static const char *const month_names[12] = {
	"Jan", "Feb", "Mar", "Apr", "May", "Jun",
	"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

/*
 * The days of each month in a year that is not a leap year.
 *
 * Values referenced: the Gregorian calendar (30 days hath September,
 * April, June and November; February has 28, or 29 in a leap year).
 */
static const uint32_t month_days[12] = {
	31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

/*
 * A time split into calendar fields in UTC, for the date of a long name.
 */
struct calendar_time {
	uint32_t year;
	uint32_t month;
	uint32_t day;
	uint32_t hour;
	uint32_t minute;
};

/*
 * The text of a long name being built into the caller's array: the
 * characters that fit are kept, the others are dropped, and the NUL is
 * written last.
 */
struct long_name_text {
	char *out;
	size_t capacity;
	size_t length;
};

static int parse_init(struct ssh_reader *reader, struct ssh_sftp_request *parsed);
static int parse_fields(struct ssh_reader *reader, struct ssh_sftp_request *parsed);
static int parse_attrs(struct ssh_reader *reader, struct ssh_sftp_attrs *attrs);
static int parse_path(struct ssh_reader *reader, const uint8_t **path, size_t *path_size);
static int parse_handle(struct ssh_reader *reader, const uint8_t **handle, size_t *handle_size);
static void fail_buffer(struct ssh_buf *out, int error);
static void put_packet_header(struct ssh_buf *out, uint8_t type, uint32_t id, size_t *start);
static void put_packet_end(struct ssh_buf *out, size_t start);
static void put_attrs_fields(const struct ssh_sftp_attrs *attrs, struct ssh_buf *out);
static void put_text(struct ssh_buf *out, const char *text);
static int is_leap_year(uint32_t year);
static void split_time(uint32_t seconds, struct calendar_time *calendar);
static void text_char(struct long_name_text *text, char character);
static void text_string(struct long_name_text *text, const char *string);
static void text_number(struct long_name_text *text, uint64_t value, size_t width);
static void text_padded(struct long_name_text *text, const char *string, size_t width);
static void text_two_digits(struct long_name_text *text, uint32_t value);
static void text_triplet(struct long_name_text *text, uint32_t bits, int special, char special_execute, char special_plain);
static void text_permissions(struct long_name_text *text, const struct ssh_sftp_attrs *attrs);
static void text_date(struct long_name_text *text, const struct ssh_sftp_attrs *attrs, int64_t now);

/*
 * Takes one whole packet from the front of input and copies the bytes
 * after its length field (type first) into packet_out, which the caller
 * reset.
 *
 * A failure to hold the bytes in packet_out (its limit, or memory) is
 * returned as the buffer recorded it, and input is left as it was.
 */
int
ssh_sftp_next_packet(
	struct ssh_buf *input,
	struct ssh_buf *packet_out)
{
	uint32_t length;
	int error;

	/* Waits until the length field is whole. */
	if (input->size < PACKET_LENGTH_SIZE)
		return EAGAIN;

	/* Reads the length of the packet that follows the field. */
	length = ssh_load_be32(input->data);

	/* Refuses a packet that has no type byte or is longer than the limit, before waiting for its bytes. */
	if (length == 0 || length > SSH_SFTP_PACKET_MAX)
		return EINVAL;

	/* Waits until the whole packet has arrived. */
	if (input->size - PACKET_LENGTH_SIZE < length)
		return EAGAIN;

	/* Copies the packet's bytes, type first, to the caller's buffer. */
	ssh_buf_put_bytes(packet_out, input->data + PACKET_LENGTH_SIZE, length);
	error = ssh_buf_error(packet_out);
	if (error != 0)
		return error;

	/* Drops the length field and the packet from the input now that they are taken. */
	ssh_buf_consume(input, PACKET_LENGTH_SIZE + length);

	/* Succeeded: one packet was moved to packet_out. */
	return 0;
}

/*
 * Reads a request packet (type first) into request.
 *
 * When the packet fails, request holds only the type and, if it could be
 * read, the id, so that the server can still answer the right request.
 */
int
ssh_sftp_parse_request(
	const uint8_t *packet,
	size_t size,
	struct ssh_sftp_request *request)
{
	struct ssh_reader reader;
	struct ssh_sftp_request parsed;
	uint8_t type;
	uint32_t id;
	int error;

	/* Starts with a request that holds nothing, which is what a failure leaves. */
	memset(request, 0, sizeof(*request));
	memset(&parsed, 0, sizeof(parsed));

	/* Refuses a packet that has no type byte. */
	if (size == 0)
		return EINVAL;

	/* Starts reading the packet and takes its type. */
	ssh_reader_init(&reader, packet, size);
	type = ssh_reader_get_u8(&reader);
	parsed.type = type;
	request->type = type;

	/* INIT carries a version in place of an id. */
	if (type == SSH_FXP_INIT) {
		error = parse_init(&reader, &parsed);
		if (error != 0)
			return error;

		/* Succeeded: the version and the ignored extensions were read. */
		*request = parsed;
		return 0;
	}

	/* Takes the request id, which every other request starts with. */
	id = ssh_reader_get_u32(&reader);
	error = ssh_reader_error(&reader);
	if (error != 0)
		return EINVAL;
	parsed.id = id;
	request->id = id;

	/* Reads the fields that the request's type has. */
	error = parse_fields(&reader, &parsed);
	if (error != 0)
		return error;

	/* Refuses bytes left over after the last field. */
	error = ssh_reader_finish(&reader);
	if (error != 0)
		return EINVAL;

	/* Succeeded: the request was read whole. */
	*request = parsed;
	return 0;
}

/*
 * Appends the SSH_FXP_VERSION packet (version SSH_SFTP_VERSION, no
 * extensions) with its length to out.
 */
void
ssh_sftp_put_version(
	struct ssh_buf *out)
{
	size_t start;

	/* Writes the length placeholder, the type, and the version in place of an id. */
	start = out->size;
	ssh_buf_put_u32(out, 0);
	ssh_buf_put_u8(out, SSH_FXP_VERSION);
	ssh_buf_put_u32(out, SSH_SFTP_VERSION);

	/* Fixes the length now that the packet is whole. */
	put_packet_end(out, start);
}

/*
 * Appends an SSH_FXP_STATUS packet for id with code, the message (an
 * English text for the code) and an empty language tag (7).
 *
 * A message of NULL is written as an empty string.
 */
void
ssh_sftp_put_status(
	uint32_t id,
	uint32_t code,
	const char *message,
	struct ssh_buf *out)
{
	size_t start;

	/* Writes the header, the status code, the message and the empty language tag. */
	put_packet_header(out, SSH_FXP_STATUS, id, &start);
	ssh_buf_put_u32(out, code);
	put_text(out, message);
	ssh_buf_put_string(out, "", 0);

	/* Fixes the length now that the packet is whole. */
	put_packet_end(out, start);
}

/*
 * Appends an SSH_FXP_HANDLE packet with size bytes of handle.
 *
 * A handle longer than the 256 bytes the draft allows records EMSGSIZE in
 * out and writes nothing.
 */
void
ssh_sftp_put_handle(
	uint32_t id,
	const uint8_t *handle,
	size_t size,
	struct ssh_buf *out)
{
	size_t start;

	/* Refuses a handle that the draft's limit does not allow, before writing any of the packet. */
	if (size > HANDLE_MAX) {
		fail_buffer(out, EMSGSIZE);
		return;
	}

	/* Writes the header and the handle as a string. */
	put_packet_header(out, SSH_FXP_HANDLE, id, &start);
	ssh_buf_put_string(out, handle, size);

	/* Fixes the length now that the packet is whole. */
	put_packet_end(out, start);
}

/*
 * Appends an SSH_FXP_DATA packet with size bytes.
 */
void
ssh_sftp_put_data(
	uint32_t id,
	const uint8_t *data,
	size_t size,
	struct ssh_buf *out)
{
	size_t start;

	/* Writes the header and the data as a string. */
	put_packet_header(out, SSH_FXP_DATA, id, &start);
	ssh_buf_put_string(out, data, size);

	/* Fixes the length now that the packet is whole. */
	put_packet_end(out, start);
}

/*
 * Appends an SSH_FXP_ATTRS packet.
 */
void
ssh_sftp_put_attrs(
	uint32_t id,
	const struct ssh_sftp_attrs *attrs,
	struct ssh_buf *out)
{
	size_t start;

	/* Writes the header and the attributes. */
	put_packet_header(out, SSH_FXP_ATTRS, id, &start);
	put_attrs_fields(attrs, out);

	/* Fixes the length now that the packet is whole. */
	put_packet_end(out, start);
}

/*
 * Starts an SSH_FXP_NAME packet for id with count entries (7); the
 * entries follow with ssh_sftp_put_name_entry and the length is fixed by
 * ssh_sftp_end_packet.
 */
void
ssh_sftp_begin_name(
	uint32_t id,
	uint32_t count,
	struct ssh_buf *out,
	size_t *start)
{
	/* Writes the header, leaving the length for the end, and the number of entries that follow. */
	put_packet_header(out, SSH_FXP_NAME, id, start);
	ssh_buf_put_u32(out, count);
}

/*
 * Appends one name entry: filename, longname (the "ls -l" form, 7),
 * attrs.
 *
 * A filename or longname of NULL is written as an empty string.
 */
void
ssh_sftp_put_name_entry(
	const char *filename,
	const char *longname,
	const struct ssh_sftp_attrs *attrs,
	struct ssh_buf *out)
{
	/* Writes the two names and the attributes of the entry. */
	put_text(out, filename);
	put_text(out, longname);
	put_attrs_fields(attrs, out);
}

/*
 * Writes the length field of the packet that starts at start in out.
 *
 * Besides EMSGSIZE for a packet over SSH_SFTP_PACKET_MAX, it returns the
 * error already recorded in out, and EINVAL for a start that is not the
 * beginning of a packet in out; on any of them the caller drops the bytes
 * from start on.
 */
int
ssh_sftp_end_packet(
	struct ssh_buf *out,
	size_t start)
{
	size_t length;
	int error;

	/* Reports a write that failed earlier, whose packet is not whole. */
	error = ssh_buf_error(out);
	if (error != 0)
		return error;

	/* Refuses a start that leaves no room for the length field and the type. */
	if (start > out->size || out->size - start < PACKET_LENGTH_SIZE + 1U)
		return EINVAL;

	/* Measures the packet after its length field. */
	length = out->size - start - PACKET_LENGTH_SIZE;

	/* Refuses a packet that the reading side would not accept. */
	if (length > SSH_SFTP_PACKET_MAX)
		return EMSGSIZE;

	/* Stores the length, which is below 2^32 because of the limit just checked. */
	ssh_store_be32(out->data + start, (uint32_t)length);

	/* Succeeded: the packet at start has its length. */
	return 0;
}

/*
 * Writes the "ls -l" long name of 7 (permissions as ten characters, link
 * count, owner, group, size, date, name) for attrs and name into out
 * (capacity bytes with the NUL; cut when longer).
 *
 * The fields are separated by one space and are at least as wide as in
 * the draft's example: the link count 3, the owner and group 8 (left
 * aligned), the size 8, the date 12.  The date is "Mon dd hh:mm" when the
 * modification time is not after now and less than six months before it,
 * else "Mon dd  yyyy"; a missing modification time is 0 and a missing mode
 * is shown as "?---------".  A NULL owner or group is shown as "?".
 */
void
ssh_sftp_long_name(
	const struct ssh_sftp_attrs *attrs,
	uint32_t links,
	const char *owner,
	const char *group,
	const char *name,
	int64_t now,
	char *out,
	size_t capacity)
{
	struct long_name_text text;
	uint64_t size;

	/* Leaves a caller that gave no room for even the NUL alone. */
	if (capacity == 0)
		return;

	/* Starts with no characters in the array. */
	text.out = out;
	text.capacity = capacity;
	text.length = 0;

	/* Takes the size when the attributes have it. */
	size = 0;
	if ((attrs->flags & SSH_FILEXFER_ATTR_SIZE) != 0)
		size = attrs->size;

	/* Writes the permissions, the link count, the owner and the group. */
	text_permissions(&text, attrs);
	text_char(&text, ' ');
	text_number(&text, links, LONG_NAME_LINKS_WIDTH);
	text_char(&text, ' ');
	if (owner == NULL)
		owner = "?";
	text_padded(&text, owner, LONG_NAME_OWNER_WIDTH);
	text_char(&text, ' ');
	if (group == NULL)
		group = "?";
	text_padded(&text, group, LONG_NAME_GROUP_WIDTH);

	/* Writes the size, the date and the name. */
	text_char(&text, ' ');
	text_number(&text, size, LONG_NAME_SIZE_WIDTH);
	text_char(&text, ' ');
	text_date(&text, attrs, now);
	text_char(&text, ' ');
	if (name == NULL)
		name = "";
	text_string(&text, name);

	/* Ends the characters that were kept. */
	out[text.length] = '\0';
}

/*
 * Reads the part of an INIT after its type: the version and any
 * extension pairs, which are skipped (the draft's section 4: names not
 * recognized are silently ignored).
 */
static int
parse_init(
	struct ssh_reader *reader,
	struct ssh_sftp_request *parsed)
{
	const uint8_t *skipped;
	size_t skipped_size;
	size_t left;
	int error;

	/* Reads the version the client speaks. */
	parsed->version = ssh_reader_get_u32(reader);

	/* Skips each extension pair: a name string and a data string. */
	left = ssh_reader_left(reader);
	while (left > 0) {
		ssh_reader_get_string(reader, &skipped, &skipped_size);
		ssh_reader_get_string(reader, &skipped, &skipped_size);
		left = ssh_reader_left(reader);
	}

	/* Refuses a packet whose version or pairs run past its end. */
	error = ssh_reader_finish(reader);
	if (error != 0)
		return EINVAL;

	/* Succeeded: the version is in parsed. */
	return 0;
}

/*
 * Reads the fields after the id, by the type of the request.
 *
 * An unknown type reads nothing more and returns ENOTSUP.
 */
static int
parse_fields(
	struct ssh_reader *reader,
	struct ssh_sftp_request *parsed)
{
	int error;

	/* Reads the fields of the request type, grouped by the shape of their fields. */
	error = 0;
	switch (parsed->type) {
	case SSH_FXP_LSTAT:
	case SSH_FXP_OPENDIR:
	case SSH_FXP_REMOVE:
	case SSH_FXP_RMDIR:
	case SSH_FXP_REALPATH:
	case SSH_FXP_STAT:
	case SSH_FXP_READLINK:
		/* A request with one path. */
		error = parse_path(reader, &parsed->path, &parsed->path_size);
		break;

	case SSH_FXP_CLOSE:
	case SSH_FXP_FSTAT:
	case SSH_FXP_READDIR:
		/* A request with one handle. */
		error = parse_handle(reader, &parsed->handle, &parsed->handle_size);
		break;

	case SSH_FXP_SETSTAT:
	case SSH_FXP_MKDIR:
		/* A request with a path and the attributes to apply to it. */
		error = parse_path(reader, &parsed->path, &parsed->path_size);
		if (error == 0)
			error = parse_attrs(reader, &parsed->attrs);
		break;

	case SSH_FXP_FSETSTAT:
		/* A request with a handle and the attributes to apply to it. */
		error = parse_handle(reader, &parsed->handle, &parsed->handle_size);
		if (error == 0)
			error = parse_attrs(reader, &parsed->attrs);
		break;

	case SSH_FXP_OPEN:
		/* A request with a path, the open flags and the attributes of a new file. */
		error = parse_path(reader, &parsed->path, &parsed->path_size);
		if (error == 0)
			parsed->pflags = ssh_reader_get_u32(reader);
		if (error == 0)
			error = parse_attrs(reader, &parsed->attrs);
		break;

	case SSH_FXP_READ:
		/*
		 * A request with a handle, an offset and a length.  A length over
		 * the most one DATA reply carries is cut to it, which the draft
		 * allows (a read returns at most the bytes asked for).
		 */
		error = parse_handle(reader, &parsed->handle, &parsed->handle_size);
		if (error == 0) {
			parsed->offset = ssh_reader_get_u64(reader);
			parsed->length = ssh_reader_get_u32(reader);
			if (parsed->length > SSH_SFTP_READ_MAX)
				parsed->length = SSH_SFTP_READ_MAX;
		}
		break;

	case SSH_FXP_WRITE:
		/* A request with a handle, an offset and the bytes to write. */
		error = parse_handle(reader, &parsed->handle, &parsed->handle_size);
		if (error == 0) {
			parsed->offset = ssh_reader_get_u64(reader);
			ssh_reader_get_string(reader, &parsed->data, &parsed->data_size);
		}
		break;

	case SSH_FXP_RENAME:
	case SSH_FXP_SYMLINK:
		/* A request with two paths, in the order they come. */
		error = parse_path(reader, &parsed->path, &parsed->path_size);
		if (error == 0)
			error = parse_path(reader, &parsed->target, &parsed->target_size);
		break;

	default:
		/* Not a request this server answers: the server replies OP_UNSUPPORTED to this id. */
		return ENOTSUP;
	}

	/* Refuses a field that ran past the end of the packet. */
	if (error == 0)
		error = ssh_reader_error(reader);
	if (error != 0)
		return EINVAL;

	/* Succeeded: the fields of the request are in parsed. */
	return 0;
}

/*
 * Reads an ATTRS (the draft's section 5) into attrs, and skips its
 * extended pairs.
 *
 * The EXTENDED bit is cleared in attrs->flags because no extended pair is
 * kept.  On a failure attrs is all zero.
 */
static int
parse_attrs(
	struct ssh_reader *reader,
	struct ssh_sftp_attrs *attrs)
{
	const uint8_t *skipped;
	size_t skipped_size;
	uint32_t flags;
	uint32_t count;
	uint32_t index;
	int error;

	/* Starts with no field present. */
	memset(attrs, 0, sizeof(*attrs));

	/* Reads the flags that say which fields follow. */
	flags = ssh_reader_get_u32(reader);
	error = ssh_reader_error(reader);
	if (error != 0)
		return EINVAL;

	/* Refuses a flag bit that version 3 does not define (the draft's section 5). */
	if ((flags & ~ATTR_FLAGS_KNOWN) != 0)
		return EINVAL;

	/* Reads the size when it is present. */
	if ((flags & SSH_FILEXFER_ATTR_SIZE) != 0)
		attrs->size = ssh_reader_get_u64(reader);

	/* Reads the owner and the group when they are present. */
	if ((flags & SSH_FILEXFER_ATTR_UIDGID) != 0) {
		attrs->uid = ssh_reader_get_u32(reader);
		attrs->gid = ssh_reader_get_u32(reader);
	}

	/* Reads the permissions when they are present. */
	if ((flags & SSH_FILEXFER_ATTR_PERMISSIONS) != 0)
		attrs->permissions = ssh_reader_get_u32(reader);

	/* Reads the access time and the modification time when they are present. */
	if ((flags & SSH_FILEXFER_ATTR_ACMODTIME) != 0) {
		attrs->atime = ssh_reader_get_u32(reader);
		attrs->mtime = ssh_reader_get_u32(reader);
	}

	/* Skips the extended pairs, as many as the count says, if there are few enough. */
	error = ssh_reader_error(reader);
	if (error == 0 && (flags & SSH_FILEXFER_ATTR_EXTENDED) != 0) {
		count = ssh_reader_get_u32(reader);
		error = ssh_reader_error(reader);
		if (error == 0 && count > SSH_SFTP_EXTENDED_MAX)
			error = EINVAL;
		for (index = 0;
		     error == 0 && index < count;
		     index++) {
			ssh_reader_get_string(reader, &skipped, &skipped_size);
			ssh_reader_get_string(reader, &skipped, &skipped_size);
			error = ssh_reader_error(reader);
		}
	}

	/* Refuses attributes that ran past the end or had too many pairs. */
	if (error != 0) {
		memset(attrs, 0, sizeof(*attrs));
		return EINVAL;
	}

	/* Keeps the flags of the fields held; the skipped extended pairs are not held. */
	attrs->flags = flags & ~SSH_FILEXFER_ATTR_EXTENDED;

	/* Succeeded: attrs holds the fields the flags name. */
	return 0;
}

/*
 * Reads a path string: at most SSH_SFTP_PATH_MAX bytes, no NUL inside.
 *
 * An empty path is valid (the draft's section 6.2).  On a failure the
 * outputs are NULL and 0.
 */
static int
parse_path(
	struct ssh_reader *reader,
	const uint8_t **path,
	size_t *path_size)
{
	const uint8_t *bytes;
	const void *found;
	size_t size;
	int error;

	/* Starts with no path. */
	*path = NULL;
	*path_size = 0;

	/* Reads the string. */
	ssh_reader_get_string(reader, &bytes, &size);
	error = ssh_reader_error(reader);
	if (error != 0)
		return EINVAL;

	/* Refuses a path longer than the limit. */
	if (size > SSH_SFTP_PATH_MAX)
		return EINVAL;

	/* Refuses a NUL inside the path, which a file system call would take as its end. */
	found = NULL;
	if (size > 0)
		found = memchr(bytes, 0, size);
	if (found != NULL)
		return EINVAL;

	/* Hands the path to the caller. */
	*path = bytes;
	*path_size = size;

	/* Succeeded: the path is inside the packet. */
	return 0;
}

/*
 * Reads a handle string: at most 256 bytes (the draft's section 6), any
 * bytes at all.  On a failure the outputs are NULL and 0.
 */
static int
parse_handle(
	struct ssh_reader *reader,
	const uint8_t **handle,
	size_t *handle_size)
{
	const uint8_t *bytes;
	size_t size;
	int error;

	/* Starts with no handle. */
	*handle = NULL;
	*handle_size = 0;

	/* Reads the string. */
	ssh_reader_get_string(reader, &bytes, &size);
	error = ssh_reader_error(reader);
	if (error != 0)
		return EINVAL;

	/* Refuses a handle longer than any this server hands out. */
	if (size > HANDLE_MAX)
		return EINVAL;

	/* Hands the handle to the caller. */
	*handle = bytes;
	*handle_size = size;

	/* Succeeded: the handle is inside the packet. */
	return 0;
}

/*
 * Records a failure in out the way a write does, so that a caller that
 * checks the buffer once sees it.
 */
static void
fail_buffer(
	struct ssh_buf *out,
	int error)
{
	/* Keeps the first failure, as the buffer does. */
	if (out->error == 0)
		out->error = error;
}

/*
 * Starts a packet of type for id: the length field (written by
 * put_packet_end), the type and the id.  *start receives the packet's
 * offset in out.
 */
static void
put_packet_header(
	struct ssh_buf *out,
	uint8_t type,
	uint32_t id,
	size_t *start)
{
	/* Remembers where the packet begins and leaves room for its length. */
	*start = out->size;
	ssh_buf_put_u32(out, 0);

	/* Writes the type and the request id the reply answers. */
	ssh_buf_put_u8(out, type);
	ssh_buf_put_u32(out, id);
}

/*
 * Fixes the length of the packet that began at start, and records a
 * failure in out when it cannot be fixed.
 */
static void
put_packet_end(
	struct ssh_buf *out,
	size_t start)
{
	int error;

	/* Writes the length field. */
	error = ssh_sftp_end_packet(out, start);
	if (error != 0)
		fail_buffer(out, error);
}

/*
 * Writes the fields of an ATTRS (the draft's section 5): the flags that
 * name a field this module writes, then those fields in the draft's order.
 * No extended pair is written.
 */
static void
put_attrs_fields(
	const struct ssh_sftp_attrs *attrs,
	struct ssh_buf *out)
{
	uint32_t flags;

	/* Keeps only the flags whose fields are written. */
	flags = attrs->flags & ATTR_FLAGS_WRITTEN;
	ssh_buf_put_u32(out, flags);

	/* Writes the size when it is present. */
	if ((flags & SSH_FILEXFER_ATTR_SIZE) != 0)
		ssh_buf_put_u64(out, attrs->size);

	/* Writes the owner and the group when they are present. */
	if ((flags & SSH_FILEXFER_ATTR_UIDGID) != 0) {
		ssh_buf_put_u32(out, attrs->uid);
		ssh_buf_put_u32(out, attrs->gid);
	}

	/* Writes the permissions when they are present. */
	if ((flags & SSH_FILEXFER_ATTR_PERMISSIONS) != 0)
		ssh_buf_put_u32(out, attrs->permissions);

	/* Writes the access time and the modification time when they are present. */
	if ((flags & SSH_FILEXFER_ATTR_ACMODTIME) != 0) {
		ssh_buf_put_u32(out, attrs->atime);
		ssh_buf_put_u32(out, attrs->mtime);
	}
}

/*
 * Writes a C string as a string, an empty one for NULL.
 */
static void
put_text(
	struct ssh_buf *out,
	const char *text)
{
	/* Writes the empty string in place of a missing text. */
	if (text == NULL) {
		ssh_buf_put_string(out, "", 0);
		return;
	}

	/* Writes the text without its NUL. */
	ssh_buf_put_cstring(out, text);
}

/*
 * Reports whether a year of the Gregorian calendar has 366 days: those
 * divisible by 4, except those divisible by 100 and not by 400.
 */
static int
is_leap_year(
	uint32_t year)
{
	/* A year divisible by 400 is a leap year. */
	if (year % 400U == 0)
		return 1;

	/* A year divisible by 100 but not by 400 is not. */
	if (year % 100U == 0)
		return 0;

	/* Any other year divisible by 4 is. */
	if (year % 4U == 0)
		return 1;

	/* The remaining years are not. */
	return 0;
}

/*
 * Splits seconds since 1970-01-01 00:00:00 UTC into calendar fields.
 * The loops run at most once per year since 1970 (about 140 for the
 * largest 32-bit time) and at most 11 times over the months.
 */
static void
split_time(
	uint32_t seconds,
	struct calendar_time *calendar)
{
	uint32_t days;
	uint32_t day_seconds;
	uint32_t year;
	uint32_t year_length;
	uint32_t month;
	uint32_t month_length;
	int leap;

	/* Separates the whole days from the seconds of the last day. */
	days = seconds / SECONDS_PER_DAY;
	day_seconds = seconds % SECONDS_PER_DAY;

	/* Takes whole years off the days until the days left are within one year. */
	year = 1970;
	leap = is_leap_year(year);
	year_length = 365;
	if (leap != 0)
		year_length = 366;
	while (days >= year_length) {
		days -= year_length;
		year++;
		leap = is_leap_year(year);
		year_length = 365;
		if (leap != 0)
			year_length = 366;
	}

	/* Takes whole months off the days until the days left are within one month. */
	month = 0;
	month_length = month_days[0];
	while (month < 11 && days >= month_length) {
		days -= month_length;
		month++;
		month_length = month_days[month];
		if (month == 1 && leap != 0)
			month_length = 29;
	}

	/* Stores the date, with the day of the month counted from 1, and the time of day. */
	calendar->year = year;
	calendar->month = month;
	calendar->day = days + 1;
	calendar->hour = day_seconds / SECONDS_PER_HOUR;
	calendar->minute = (day_seconds % SECONDS_PER_HOUR) / SECONDS_PER_MINUTE;
}

/*
 * Appends one character to the long name if it fits before the NUL.
 */
static void
text_char(
	struct long_name_text *text,
	char character)
{
	/* Drops a character that would leave no room for the NUL. */
	if (text->length + 1U >= text->capacity)
		return;

	/* Keeps the character. */
	text->out[text->length] = character;
	text->length++;
}

/*
 * Appends a C string to the long name, as much as fits.
 */
static void
text_string(
	struct long_name_text *text,
	const char *string)
{
	size_t index;

	/* Appends each character of the string. */
	for (index = 0;
	     string[index] != '\0';
	     index++)
		text_char(text, string[index]);
}

/*
 * Appends an unsigned number in decimal, padded on the left with spaces
 * to at least width characters.
 */
static void
text_number(
	struct long_name_text *text,
	uint64_t value,
	size_t width)
{
	char digits[24];
	size_t count;
	size_t index;

	/* Writes the digits, least significant first, at least one. */
	count = 0;
	do {
		digits[count] = (char)('0' + (int)(value % 10U));
		count++;
		value /= 10U;
	} while (value != 0);

	/* Pads on the left up to the width. */
	for (index = count;
	     index < width;
	     index++)
		text_char(text, ' ');

	/* Writes the digits, most significant first. */
	for (index = count;
	     index > 0;
	     index--)
		text_char(text, digits[index - 1U]);
}

/*
 * Appends a string, padded on the right with spaces to at least width
 * characters.
 */
static void
text_padded(
	struct long_name_text *text,
	const char *string,
	size_t width)
{
	size_t length;

	/* Writes the string itself. */
	text_string(text, string);

	/* Pads on the right up to the width. */
	for (length = strlen(string);
	     length < width;
	     length++)
		text_char(text, ' ');
}

/*
 * Appends a number below 100 as two digits, with a leading zero.
 */
static void
text_two_digits(
	struct long_name_text *text,
	uint32_t value)
{
	/* Writes the tens digit and then the ones digit. */
	text_char(text, (char)('0' + (int)((value / 10U) % 10U)));
	text_char(text, (char)('0' + (int)(value % 10U)));
}

/*
 * Appends the three characters of one class of users: r, w and then x
 * (bits 4, 2 and 1 of bits), where the execute position shows
 * special_execute if the special bit is set and the execute bit is too,
 * special_plain if the special bit is set without the execute bit.
 */
static void
text_triplet(
	struct long_name_text *text,
	uint32_t bits,
	int special,
	char special_execute,
	char special_plain)
{
	char read_mark;
	char write_mark;
	char execute_mark;

	/* Shows the read and write bits. */
	read_mark = '-';
	if ((bits & 4U) != 0)
		read_mark = 'r';
	write_mark = '-';
	if ((bits & 2U) != 0)
		write_mark = 'w';

	/* Shows the execute bit, as the special letter when the special bit is set. */
	execute_mark = '-';
	if ((bits & 1U) != 0)
		execute_mark = 'x';
	if (special != 0) {
		if ((bits & 1U) != 0)
			execute_mark = special_execute;
		else
			execute_mark = special_plain;
	}

	/* Writes the three marks. */
	text_char(text, read_mark);
	text_char(text, write_mark);
	text_char(text, execute_mark);
}

/*
 * Appends the ten characters of the permissions: the kind of file, then
 * read, write and execute for user, group and others, with s, S, t and T
 * for the setuid, setgid and sticky bits.  A mode that is missing or of an
 * unknown kind starts with a question mark.
 */
static void
text_permissions(
	struct long_name_text *text,
	const struct ssh_sftp_attrs *attrs)
{
	uint32_t mode;
	char kind;

	/* Takes the mode, which is 0 when the attributes do not have it. */
	mode = 0;
	if ((attrs->flags & SSH_FILEXFER_ATTR_PERMISSIONS) != 0)
		mode = attrs->permissions;

	/* Names the kind of file. */
	kind = '?';
	switch (mode & MODE_TYPE_MASK) {
	case MODE_TYPE_SOCKET:
		kind = 's';
		break;
	case MODE_TYPE_SYMLINK:
		kind = 'l';
		break;
	case MODE_TYPE_REGULAR:
		kind = '-';
		break;
	case MODE_TYPE_BLOCK:
		kind = 'b';
		break;
	case MODE_TYPE_DIRECTORY:
		kind = 'd';
		break;
	case MODE_TYPE_CHARACTER:
		kind = 'c';
		break;
	case MODE_TYPE_FIFO:
		kind = 'p';
		break;
	default:
		break;
	}

	/* Writes the kind, then user, group and others, each with its special bit. */
	text_char(text, kind);
	text_triplet(text, (mode >> 6) & 7U, (mode & MODE_SETUID) != 0, 's', 'S');
	text_triplet(text, (mode >> 3) & 7U, (mode & MODE_SETGID) != 0, 's', 'S');
	text_triplet(text, mode & 7U, (mode & MODE_STICKY) != 0, 't', 'T');
}

/*
 * Appends the 12 characters of the modification time: "Mon dd hh:mm" when
 * it is not after now and less than RECENT_SECONDS before it, else
 * "Mon dd  yyyy".
 */
static void
text_date(
	struct long_name_text *text,
	const struct ssh_sftp_attrs *attrs,
	int64_t now)
{
	struct calendar_time calendar;
	uint32_t mtime;
	uint64_t age;
	int recent;

	/* Takes the modification time, which is 0 when the attributes do not have it. */
	mtime = 0;
	if ((attrs->flags & SSH_FILEXFER_ATTR_ACMODTIME) != 0)
		mtime = attrs->mtime;

	/*
	 * Decides whether the time of day is shown.  A time after now is not
	 * recent; the age is then computed from two non-negative values, so
	 * it cannot overflow.
	 */
	recent = 0;
	if (now >= (int64_t)mtime) {
		age = (uint64_t)now - (uint64_t)mtime;
		if (age < RECENT_SECONDS)
			recent = 1;
	}

	/* Splits the time into its calendar fields. */
	split_time(mtime, &calendar);

	/* Writes the month and the day of the month, padded with a space. */
	text_string(text, month_names[calendar.month]);
	text_char(text, ' ');
	text_number(text, calendar.day, 2);
	text_char(text, ' ');

	/* Writes the time of day when it is recent, else the year. */
	if (recent != 0) {
		text_two_digits(text, calendar.hour);
		text_char(text, ':');
		text_two_digits(text, calendar.minute);
	} else {
		text_number(text, calendar.year, 5);
	}
}
