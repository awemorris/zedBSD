/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A unit's tarball, unpacked (unpack.h).
 *
 * The tarball is read whole from the descriptor it will be unpacked from,
 * its SHA-256 taken while it is read, and compared with the index's before
 * anything of it is believed; it is never read a second time, so it cannot
 * be changed between the check and the use.  libz-compat's inflate takes a
 * stream whole and gives its output whole, so the unpacked archive is held
 * in memory too.
 *
 * The gzip wrapper (RFC 1952) is read here, for libz-compat reads only the
 * zlib one: the header, the raw deflate stream, and the trailer's CRC-32
 * and size.  The archive is ustar (POSIX.1-1988, the "ustar\0" "00" magic):
 * regular files, directories and symbolic links only; the owner in a
 * header is not used (what the helper makes is root's), and a set-user-ID
 * or set-group-ID bit is kept only where the index allows it.  A path is
 * relative, without empty, "." or ".." components, and each component is
 * opened with O_NOFOLLOW from the one before, so an entry cannot be written
 * through a symbolic link an earlier entry made.
 */

#include "unpack.h"

#include "userland/base/common/sha256.h"

#include <compat/zlib/zlib.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The ustar block, and where its fields are (POSIX.1, pax "ustar Interchange Format"). */
#define UNPACK_BLOCK		512U
#define UNPACK_NAME		0U
#define UNPACK_NAME_SIZE	100U
#define UNPACK_MODE		100U
#define UNPACK_MODE_SIZE	8U
#define UNPACK_SIZE		124U
#define UNPACK_SIZE_SIZE	12U
#define UNPACK_CHECKSUM		148U
#define UNPACK_CHECKSUM_SIZE	8U
#define UNPACK_TYPE		156U
#define UNPACK_LINK		157U
#define UNPACK_LINK_SIZE	100U
#define UNPACK_MAGIC		257U
#define UNPACK_PREFIX		345U
#define UNPACK_PREFIX_SIZE	155U

/* The gzip header's fixed part and its flags (RFC 1952, 2.3.1). */
#define UNPACK_GZIP_HEADER	10U
#define UNPACK_GZIP_TRAILER	8U
#define UNPACK_GZIP_FHCRC	0x02U
#define UNPACK_GZIP_FEXTRA	0x04U
#define UNPACK_GZIP_FNAME	0x08U
#define UNPACK_GZIP_FCOMMENT	0x10U
#define UNPACK_GZIP_RESERVED	0xe0U

/* How much of the tarball one read asks for. */
#define UNPACK_READ_CHUNK	(1024U * 1024U)

/* The longest path an entry may name (the prefix, a slash and the name). */
#define UNPACK_PATH_MAX		(UNPACK_PREFIX_SIZE + 1U + UNPACK_NAME_SIZE + 1U)

static int unpack_octal(const uint8_t *field, size_t size, uint64_t *value);
static int unpack_header_checksum(const uint8_t *header);
static int unpack_zero_block(const uint8_t *block);
static int unpack_entry_path(const uint8_t *header, char *path, size_t capacity);
static int unpack_path_valid(const char *path);
static int unpack_open_parent(int root, char *path, const char **leaf);
static int unpack_make_file(int parent, const char *leaf, const uint8_t *data, uint64_t size, unsigned mode);
static int unpack_make_directory(int parent, const char *leaf, unsigned mode);
static uint32_t unpack_le32(const uint8_t *bytes);

/*
 * Reads a whole file of an expected size from a descriptor and checks its
 * SHA-256.  Returns the bytes, which the caller frees, or EIO when the
 * size differs and EBADMSG when the digest does.
 */
int
unpack_read_whole(
	int fd,
	uint64_t expected_bytes,
	const uint8_t expected[32],
	uint8_t **data,
	size_t *length)
{
	struct command_sha256_context context;
	uint8_t digest[32];
	uint8_t probe;
	uint8_t *buffer;
	size_t offset;
	size_t want;
	size_t buffer_size;
	ssize_t got;
	int difference;
	int error;

	/* Clears the results until the file is read and checked. */
	*data = NULL;
	*length = 0;

	/* A size the memory can hold, with the byte past it to see the end. */
	if (expected_bytes > (uint64_t)SIZE_MAX - 1U)
		return EFBIG;
	buffer_size = 1U;
	if (expected_bytes != 0U)
		buffer_size = (size_t)expected_bytes;
	buffer = malloc(buffer_size);
	if (buffer == NULL)
		return ENOMEM;
	command_sha256_init(&context);

	/* Reads the expected size, taking the digest as it comes. */
	offset = 0;
	while (offset < expected_bytes) {
		want = (size_t)expected_bytes - offset;
		if (want > UNPACK_READ_CHUNK)
			want = UNPACK_READ_CHUNK;
		got = read(fd, buffer + offset, want);
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0) {
			free(buffer);
			return EIO;
		}

		/* Takes the bytes into the digest. */
		error = command_sha256_update(&context, buffer + offset, (size_t)got);
		if (error != 0) {
			free(buffer);
			return EIO;
		}

		/* Counts the bytes taken. */
		offset += (size_t)got;
	}

	/* The file must end there. */
	got = read(fd, &probe, 1U);
	if (got != 0) {
		free(buffer);
		return EIO;
	}

	/* The digest must be the index's. */
	command_sha256_final(&context, digest);
	difference = memcmp(digest, expected, sizeof(digest));
	if (difference != 0) {
		free(buffer);
		return EBADMSG;
	}

	/* Hands the bytes to the caller. */
	*data = buffer;
	*length = (size_t)expected_bytes;

	/* Succeeded: the file is read and checked. */
	return 0;
}

/*
 * Undoes a gzip wrapper of one member: returns the inflated bytes, which
 * the caller frees.  A stream that inflates to more than limit bytes, one
 * whose CRC-32 or size disagrees with its trailer, or a malformed header
 * is refused with EINVAL.
 */
int
unpack_gunzip(
	const uint8_t *data,
	size_t length,
	uint64_t limit,
	uint8_t **output,
	size_t *output_length)
{
	z_stream stream;
	uint8_t *buffer;
	size_t offset;
	size_t stream_end;
	uint32_t expected_crc;
	uint32_t expected_size;
	uLong crc;
	uint8_t flags;
	unsigned extra;
	int inflate_status;

	/* Clears the results until the archive is inflated and checked. */
	*output = NULL;
	*output_length = 0;

	/* The fixed header, deflate, and no reserved flag. */
	if (length < UNPACK_GZIP_HEADER + UNPACK_GZIP_TRAILER)
		return EINVAL;
	if (data[0] != 0x1fU || data[1] != 0x8bU || data[2] != 8U)
		return EINVAL;
	flags = data[3];
	if ((flags & UNPACK_GZIP_RESERVED) != 0U)
		return EINVAL;
	offset = UNPACK_GZIP_HEADER;
	stream_end = length - UNPACK_GZIP_TRAILER;

	/* Steps over the optional extra field, name, comment and header CRC. */
	if ((flags & UNPACK_GZIP_FEXTRA) != 0U) {
		if (stream_end - offset < 2U)
			return EINVAL;
		extra = (unsigned)data[offset] | (unsigned)data[offset + 1U] << 8;
		offset += 2U;
		if (stream_end - offset < extra)
			return EINVAL;
		offset += extra;
	}

	/* The name ends at its NUL. */
	if ((flags & UNPACK_GZIP_FNAME) != 0U) {
		while (offset < stream_end && data[offset] != 0U)
			offset++;
		if (offset == stream_end)
			return EINVAL;
		offset++;
	}

	/* The comment ends at its NUL. */
	if ((flags & UNPACK_GZIP_FCOMMENT) != 0U) {
		while (offset < stream_end && data[offset] != 0U)
			offset++;
		if (offset == stream_end)
			return EINVAL;
		offset++;
	}

	/* The header's CRC-16 is two bytes. */
	if ((flags & UNPACK_GZIP_FHCRC) != 0U) {
		if (stream_end - offset < 2U)
			return EINVAL;
		offset += 2U;
	}

	/* The trailer: the CRC-32 and the size modulo 2^32 of what inflates. */
	expected_crc = unpack_le32(data + stream_end);
	expected_size = unpack_le32(data + stream_end + 4U);
	if (expected_size > limit || stream_end - offset > UINT_MAX)
		return EINVAL;

	/* Room for the size and one byte more, to see a longer stream. */
	buffer = malloc((size_t)expected_size + 1U);
	if (buffer == NULL)
		return ENOMEM;

	/* Inflates the raw stream whole (libz-compat decodes it in one go). */
	memset(&stream, 0, sizeof(stream));
	inflate_status = inflateInit2(&stream, -15);
	if (inflate_status != Z_OK) {
		free(buffer);
		return ENOMEM;
	}

	/* Gives the stream its input and its room for the output. */
	stream.next_in = (Bytef *)(uintptr_t)(data + offset);
	stream.avail_in = (uInt)(stream_end - offset);
	stream.next_out = buffer;
	stream.avail_out = (uInt)expected_size + 1U;
	inflate_status = inflate(&stream, Z_FINISH);
	if (inflate_status != Z_STREAM_END || stream.total_out != expected_size) {
		(void)inflateEnd(&stream);
		free(buffer);
		return EINVAL;
	}

	/* Ends the stream. */
	(void)inflateEnd(&stream);

	/* The CRC-32 of what inflated must be the trailer's. */
	crc = crc32(0L, Z_NULL, 0);
	crc = crc32(crc, buffer, (uInt)expected_size);
	if ((uint32_t)crc != expected_crc) {
		free(buffer);
		return EINVAL;
	}

	/* Hands the archive to the caller. */
	*output = buffer;
	*output_length = expected_size;

	/* Succeeded: the archive is inflated. */
	return 0;
}

/*
 * Unpacks a ustar archive under a root directory.  Each entry is checked
 * before anything of it is made: its header's checksum, its kind, its
 * path, its size within the archive, and a set-ID bit against the index.
 * done is told of each entry made.  The first failure stops the
 * unpacking; what was made before it stays.
 */
int
unpack_tar(
	int root,
	const uint8_t *archive,
	size_t length,
	const struct installer_index *index,
	unsigned unit,
	unpack_entry_done done,
	void *context)
{
	char path[UNPACK_PATH_MAX];
	char installed[UNPACK_PATH_MAX + 1U];
	char target[UNPACK_LINK_SIZE + 1U];
	const uint8_t *header;
	const char *leaf;
	enum unpack_kind kind;
	uint64_t size;
	uint64_t mode;
	size_t offset;
	size_t padded;
	unsigned permissions;
	int parent;
	int zero;
	int difference;
	int linked;
	int error;

	/* Walks the entries until the two zero blocks that end the archive. */
	offset = 0;
	for (;;) {
		if (length - offset < UNPACK_BLOCK)
			return EINVAL;
		header = archive + offset;
		zero = unpack_zero_block(header);
		if (zero)
			break;

		/* The header's own checksum and the POSIX magic. */
		error = unpack_header_checksum(header);
		if (error != 0)
			return error;
		difference = memcmp(header + UNPACK_MAGIC, "ustar\0" "00", 8U);
		if (difference != 0)
			return EINVAL;

		/* The kind: a regular file, a directory or a symbolic link, nothing else. */
		if (header[UNPACK_TYPE] == '0' || header[UNPACK_TYPE] == '\0')
			kind = UNPACK_FILE;
		else if (header[UNPACK_TYPE] == '5')
			kind = UNPACK_DIRECTORY;
		else if (header[UNPACK_TYPE] == '2')
			kind = UNPACK_SYMLINK;
		else
			return EINVAL;

		/* The mode and the size, in octal. */
		error = unpack_octal(header + UNPACK_MODE, UNPACK_MODE_SIZE, &mode);
		if (error == 0)
			error = unpack_octal(header + UNPACK_SIZE, UNPACK_SIZE_SIZE, &size);
		if (error != 0 || mode > 07777U)
			return EINVAL;
		permissions = (unsigned)mode;
		if (kind != UNPACK_FILE && size != 0U)
			return EINVAL;

		/* The data has to be in the archive, padded to a whole block. */
		offset += UNPACK_BLOCK;
		if (size > length - offset)
			return EINVAL;
		padded = ((size_t)size + UNPACK_BLOCK - 1U) / UNPACK_BLOCK * UNPACK_BLOCK;
		if (padded > length - offset)
			return EINVAL;

		/* The path, which must be relative and plain. */
		error = unpack_entry_path(header, path, sizeof(path));
		if (error != 0)
			return error;
		installed[0] = '/';
		memcpy(installed + 1, path, strlen(path) + 1U);

		/* A set-ID bit only where the index allows it, and only on a file. */
		if ((permissions & 06000U) != 0U) {
			if (kind != UNPACK_FILE)
				return EPERM;
			error = index_setid_allowed(index, unit, installed, permissions);
			if (error != 0)
				return error;
		}

		/* The directory the entry goes in, reached without following a link. */
		parent = unpack_open_parent(root, path, &leaf);
		if (parent < 0)
			return -parent;

		/* Makes the entry. */
		if (kind == UNPACK_FILE) {
			error = unpack_make_file(parent, leaf, archive + offset, size, permissions);
		} else if (kind == UNPACK_DIRECTORY) {
			error = unpack_make_directory(parent, leaf, permissions);
		} else {
			memcpy(target, header + UNPACK_LINK, UNPACK_LINK_SIZE);
			target[UNPACK_LINK_SIZE] = '\0';
			error = 0;
			if (target[0] == '\0')
				error = EINVAL;
			if (error == 0) {
				linked = symlinkat(target, parent, leaf);
				if (linked != 0)
					error = errno;
			}
		}

		/* Closes the directory the entry went in. */
		(void)close(parent);
		if (error != 0)
			return error;

		/* Tells the caller, who may stop here. */
		offset += padded;
		if (done != NULL) {
			error = done(context, installed, kind, size);
			if (error != 0)
				return error;
		}
	}

	/* Succeeded: every entry is made. */
	return 0;
}

/* Reads an octal field: digits, then spaces or NULs. */
static int
unpack_octal(
	const uint8_t *field,
	size_t size,
	uint64_t *value)
{
	uint64_t result;
	size_t i;

	/* Skips the leading spaces, which are allowed. */
	result = 0;
	i = 0;
	while (i < size && field[i] == ' ')
		i++;
	if (i == size || field[i] < '0' || field[i] > '7')
		return EINVAL;

	/* Adds each digit, refusing a value past 64 bits. */
	while (i < size && field[i] >= '0' && field[i] <= '7') {
		if (result > (UINT64_MAX >> 3))
			return EINVAL;
		result = result << 3 | (uint64_t)(field[i] - '0');
		i++;
	}

	/* Only spaces and NULs may follow. */
	while (i < size) {
		if (field[i] != ' ' && field[i] != '\0')
			return EINVAL;
		i++;
	}

	/* Stores the value. */
	*value = result;

	/* Succeeded: the field is read. */
	return 0;
}

/* Checks a header against its checksum, the sum of its bytes with the field as spaces. */
static int
unpack_header_checksum(
	const uint8_t *header)
{
	uint64_t stored;
	uint64_t sum;
	unsigned i;
	int error;

	/* The stored sum. */
	error = unpack_octal(header + UNPACK_CHECKSUM, UNPACK_CHECKSUM_SIZE, &stored);
	if (error != 0)
		return EINVAL;

	/* The sum of the header, the checksum's own bytes counted as spaces. */
	sum = 0;
	for (i = 0; i < UNPACK_BLOCK; i++) {
		if (i >= UNPACK_CHECKSUM && i < UNPACK_CHECKSUM + UNPACK_CHECKSUM_SIZE)
			sum += ' ';
		else
			sum += header[i];
	}

	/* A header whose sum differs is damaged. */
	if (sum != stored)
		return EINVAL;

	/* Succeeded: the header is whole. */
	return 0;
}

/* Tells whether a block is all zero bytes. */
static int
unpack_zero_block(
	const uint8_t *block)
{
	unsigned i;

	/* Any byte that is not zero. */
	for (i = 0; i < UNPACK_BLOCK; i++) {
		if (block[i] != 0U)
			return 0;
	}

	/* Every byte is zero. */
	return 1;
}

/*
 * Gives an entry's path: the prefix and the name joined by a slash, the
 * trailing slash of a directory taken off, and checked to be plain.
 */
static int
unpack_entry_path(
	const uint8_t *header,
	char *path,
	size_t capacity)
{
	char name[UNPACK_NAME_SIZE + 1U];
	char prefix[UNPACK_PREFIX_SIZE + 1U];
	size_t size;
	size_t name_size;
	int error;

	/* The name and the prefix, each NUL-terminated or filling its field. */
	memcpy(name, header + UNPACK_NAME, UNPACK_NAME_SIZE);
	name[UNPACK_NAME_SIZE] = '\0';
	memcpy(prefix, header + UNPACK_PREFIX, UNPACK_PREFIX_SIZE);
	prefix[UNPACK_PREFIX_SIZE] = '\0';
	if (name[0] == '\0')
		return EINVAL;

	/* Joins them. */
	path[0] = '\0';
	if (prefix[0] != '\0') {
		memcpy(path, prefix, strlen(prefix) + 1U);
		size = strlen(path);
		path[size] = '/';
		path[size + 1U] = '\0';
	}

	/* Appends the name when it fits. */
	size = strlen(path);
	name_size = strlen(name);
	if (size + name_size + 1U > capacity)
		return EINVAL;
	memcpy(path + size, name, name_size + 1U);

	/* A directory's name may end in one slash. */
	size = strlen(path);
	if (size > 1U && path[size - 1U] == '/')
		path[size - 1U] = '\0';

	/* The path must be relative and plain. */
	error = unpack_path_valid(path);
	if (error != 0)
		return error;

	/* Succeeded: the path is plain. */
	return 0;
}

/* Tells whether a path is relative and has no empty, "." or ".." component. */
static int
unpack_path_valid(
	const char *path)
{
	const char *component;
	const char *end;
	size_t size;

	/* Not absolute and not empty. */
	if (path[0] == '\0' || path[0] == '/')
		return EINVAL;

	/* Looks at each component between slashes. */
	component = path;
	for (;;) {
		end = strchr(component, '/');
		if (end == NULL)
			size = strlen(component);
		else
			size = (size_t)(end - component);
		if (size == 0U || size > NAME_MAX)
			return EINVAL;
		if (size == 1U && component[0] == '.')
			return EINVAL;
		if (size == 2U && component[0] == '.' && component[1] == '.')
			return EINVAL;
		if (end == NULL)
			break;
		component = end + 1;
	}

	/* Succeeded: the path is plain. */
	return 0;
}

/*
 * Opens the directory an entry goes in, one component at a time from the
 * root, refusing a component that is a symbolic link or not a directory.
 * Returns the descriptor and the last component, or a negated errno.  The
 * path is cut in place.
 */
static int
unpack_open_parent(
	int root,
	char *path,
	const char **leaf)
{
	char *component;
	char *slash;
	int directory;
	int next;
	int saved;

	/* Starts from a descriptor of its own on the root. */
	directory = openat(root, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (directory < 0)
		return -errno;

	/* Opens each component but the last. */
	component = path;
	for (;;) {
		slash = strchr(component, '/');
		if (slash == NULL)
			break;
		*slash = '\0';
		next = openat(directory, component, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		saved = errno;
		*slash = '/';
		(void)close(directory);
		if (next < 0)
			return -saved;
		directory = next;
		component = slash + 1;
	}

	/* Reports the last component. */
	*leaf = component;

	/* Succeeded: the parent is open. */
	return directory;
}

/* Makes a regular file that did not exist, writes it and gives it its mode. */
static int
unpack_make_file(
	int parent,
	const char *leaf,
	const uint8_t *data,
	uint64_t size,
	unsigned mode)
{
	uint64_t written;
	ssize_t result;
	int fd;
	int changed;
	int closed;
	int error;

	/* A new file only: a unit does not replace another unit's file. */
	fd = openat(parent, leaf, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0)
		return errno;

	/* Writes the data. */
	written = 0;
	error = 0;
	while (written < size) {
		result = write(fd, data + written, (size_t)(size - written));
		if (result < 0 && errno == EINTR)
			continue;
		if (result <= 0) {
			/* A failed write reports why, one that took nothing is an I/O error. */
			if (result < 0)
				error = errno;
			else
				error = EIO;
			break;
		}

		/* Counts the bytes written. */
		written += (uint64_t)result;
	}

	/* The mode, set-ID bits included, once the data is in. */
	if (error == 0) {
		changed = fchmod(fd, (mode_t)mode);
		if (changed != 0)
			error = errno;
	}

	/* Closes the file, whose failure counts when nothing failed before. */
	closed = close(fd);
	if (closed != 0 && error == 0)
		error = errno;

	/* Reports why the file could not be made. */
	if (error != 0)
		return error;

	/* Succeeded: the file is made. */
	return 0;
}

/* Makes a directory, or takes one an earlier unit made, and gives it its mode. */
static int
unpack_make_directory(
	int parent,
	const char *leaf,
	unsigned mode)
{
	int fd;
	int made;
	int changed;
	int error;

	/* A new directory, or an existing one that is not a link. */
	made = mkdirat(parent, leaf, 0700);
	if (made != 0 && errno != EEXIST)
		return errno;
	fd = openat(parent, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return errno;

	/* The mode the unit gives it. */
	error = 0;
	changed = fchmod(fd, (mode_t)mode);
	if (changed != 0)
		error = errno;
	(void)close(fd);
	if (error != 0)
		return error;

	/* Succeeded: the directory is there with its mode. */
	return 0;
}

/* Reads a little-endian 32-bit value. */
static uint32_t
unpack_le32(
	const uint8_t *bytes)
{
	uint32_t value;

	/* The four bytes, least significant first. */
	value = (uint32_t)bytes[0];
	value |= (uint32_t)bytes[1] << 8;
	value |= (uint32_t)bytes[2] << 16;
	value |= (uint32_t)bytes[3] << 24;

	/* The value. */
	return value;
}
