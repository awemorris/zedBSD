/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The system the helper installs (payload.h): its download, the check of
 * the whole against the release's SHA-256, and the reading of its members.
 * Once the whole is checked, the licences the index names are taken out
 * of their units (each checked against the index's SHA-256 before it is
 * inflated) for the installer.
 *
 * The download is fetch's (/bin/fetch, https with the system's roots and
 * the settings' CA file), run as a child with an empty environment, its
 * output read here: the bytes are written to a file only the helper's
 * directory holds and taken into the SHA-256 as they come.  Only a file
 * whose size and SHA-256 are the release's is renamed to the tarball;
 * nothing of it is read before.  The headers are ustar's (POSIX.1, pax
 * "ustar Interchange Format"): regular files and directories only, a path
 * relative and without "." or ".." components.
 */

#include "payload.h"
#include "unpack.h"

#include "userland/base/common/sha256.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

/* The program that downloads, and the owner the helper's files must have. */
#ifndef PAYLOAD_FETCH
#define PAYLOAD_FETCH		"/bin/fetch"
#endif
#ifndef PAYLOAD_OWNER
#define PAYLOAD_OWNER		0
#endif

/* The ustar block and where its fields are. */
#define PAYLOAD_BLOCK		512U
#define PAYLOAD_NAME		0U
#define PAYLOAD_NAME_SIZE	100U
#define PAYLOAD_SIZE		124U
#define PAYLOAD_SIZE_SIZE	12U
#define PAYLOAD_CHECKSUM	148U
#define PAYLOAD_CHECKSUM_SIZE	8U
#define PAYLOAD_TYPE		156U
#define PAYLOAD_MAGIC		257U
#define PAYLOAD_PREFIX		345U
#define PAYLOAD_PREFIX_SIZE	155U

/* The seconds the release's description may take, and the seconds a download may go without a byte. */
#define PAYLOAD_API_SECONDS	60U
#define PAYLOAD_STALL_SECONDS	120U

/* The bytes read at a time, and the bytes between two BYTES lines. */
#define PAYLOAD_CHUNK		(256U * 1024U)
#define PAYLOAD_REPORT_BYTES	(4ULL * 1024ULL * 1024ULL)

/* What a download is told to stop by: a word on its input, or the input's end. */
#define PAYLOAD_CANCELED	(-1)

/* The room a unit's inflated archive may take beyond its files: a block of header and padding each, and the end. */
#define PAYLOAD_ENTRY_ROOM	1024U
#define PAYLOAD_ARCHIVE_SLACK	(1024ULL * 1024ULL)

/* The most fields of an index record the licences' reading splits. */
#define PAYLOAD_RECORD_FIELDS	5U

/* One licence the index names: the unit it is in, and its path in the unit (without the leading slash). */
struct payload_license {
	unsigned unit;
	char path[PAYLOAD_NAME_MAX];
};

static int payload_file(const char *path, int *fd, uint64_t *size);
static int payload_header(const uint8_t *header, char *name, uint64_t *bytes, int *regular);
static int payload_octal(const uint8_t *field, size_t size, uint64_t *value);
static int payload_zero(const uint8_t *block);
static int payload_name_valid(const char *name);
static int payload_settings(struct release_config *config);
static int payload_spawn(char *const argument[], int *output, pid_t *child);
static int payload_finish(pid_t child, int stop);
static int payload_input(int input);
static int payload_describe(FILE *output, int input, const struct release_config *config, char **text, size_t *length);
static int payload_fetch(FILE *output, int input, const struct release_config *config, const struct release_asset *asset);
static int payload_keep(void);
static int payload_licenses(const struct payload *payload, const struct installer_index *index, const char *text);
static int payload_license_records(const struct installer_index *index, const char *text, struct payload_license *license, unsigned *count);
static int payload_license_unit(const struct payload *payload, const struct installer_index *index, unsigned unit, const struct payload_license *license, unsigned count, int directory);
static int payload_license_write(int directory, unsigned number, const uint8_t *data, uint64_t bytes);
static void payload_forget_licenses(void);
static void payload_fail(FILE *output, const char *step, int error, const char *text);
static uint64_t payload_seconds(void);

/*
 * Reads the members of a tarball of size bytes from its headers.  Returns
 * 0, or EINVAL for a tarball that is not ustar of regular files and
 * directories with valid names, whose members leave it, or that names a
 * member twice; EFBIG for one of too many members.
 */
int
payload_scan(
	int fd,
	uint64_t size,
	struct payload *payload)
{
	uint8_t header[PAYLOAD_BLOCK];
	char name[PAYLOAD_NAME_MAX];
	const struct payload_member *found;
	uint64_t offset;
	uint64_t bytes;
	uint64_t room;
	ssize_t count;
	int regular;
	int empty;
	int error;

	/* Each header, from the start, until the end block or the end of the file. */
	memset(payload, 0, sizeof(*payload));
	payload->size = size;
	offset = 0;
	while (offset + PAYLOAD_BLOCK <= size) {
		count = pread(fd, header, sizeof(header), (off_t)offset);
		if (count < 0 && errno == EINTR)
			continue;
		if (count != (ssize_t)sizeof(header))
			return EIO;

		/* A block of zeros ends the archive. */
		empty = payload_zero(header);
		if (empty)
			break;

		/* The member the header names, and its bytes, which must lie within the file. */
		error = payload_header(header, name, &bytes, &regular);
		if (error != 0)
			return error;
		offset += PAYLOAD_BLOCK;
		room = size - offset;
		if (bytes > room)
			return EINVAL;

		/* A regular file is kept, once; a directory has no bytes. */
		if (regular) {
			error = payload_find(payload, name, &found);
			if (error == 0)
				return EINVAL;
			if (payload->count == PAYLOAD_MEMBERS_MAX)
				return EFBIG;
			(void)memcpy(payload->member[payload->count].name, name, sizeof(name));
			payload->member[payload->count].offset = offset;
			payload->member[payload->count].bytes = bytes;
			payload->count++;
		} else if (bytes != 0U) {
			return EINVAL;
		}

		/* The next header, past the member's blocks. */
		bytes = (bytes + PAYLOAD_BLOCK - 1U) / PAYLOAD_BLOCK * PAYLOAD_BLOCK;
		if (bytes > size - offset)
			return EINVAL;
		offset += bytes;
	}

	/* Succeeded: the members are known. */
	return 0;
}

/* Finds a member by its name.  Returns 0 and the member, or ENOENT. */
int
payload_find(
	const struct payload *payload,
	const char *name,
	const struct payload_member **member)
{
	unsigned i;
	int differs;

	/* Compares the name with each member's. */
	for (i = 0; i < payload->count; i++) {
		differs = strcmp(payload->member[i].name, name);
		if (differs != 0)
			continue;

		/* The member of that name. */
		*member = &payload->member[i];
		return 0;
	}

	/* No member has that name. */
	return ENOENT;
}

/*
 * Reads the members of the downloaded tarball and its index into text (at
 * most capacity bytes with a NUL) and index.  Returns 0, ENOENT when no
 * system has been downloaded, EPERM for a tarball that is not root's
 * alone, or the error of reading it.
 */
int
payload_load(
	struct payload *payload,
	struct installer_index *index,
	char *text,
	size_t capacity)
{
	const struct payload_member *member;
	uint64_t size;
	ssize_t count;
	size_t length;
	int fd;
	int error;

	/* The tarball, root's alone. */
	error = payload_file(PAYLOAD_TARBALL, &fd, &size);
	if (error != 0)
		return error;

	/* Its members, then the index among them. */
	error = payload_scan(fd, size, payload);
	if (error == 0)
		error = payload_find(payload, "index", &member);
	if (error == 0 && member->bytes >= capacity)
		error = EFBIG;
	if (error != 0) {
		close(fd);
		return error;
	}

	/* The index's bytes, whole. */
	length = (size_t)member->bytes;
	count = pread(fd, text, length, (off_t)member->offset);
	close(fd);
	if (count != (ssize_t)length)
		return EIO;
	text[length] = '\0';

	/* Its records. */
	error = index_parse(text, length, index);
	if (error != 0)
		return error;

	/* Succeeded: the tarball and its index are read. */
	return 0;
}

/*
 * Opens the tarball at a member of the size the index gives, for one read
 * of that many bytes.  Returns 0 with the descriptor (the caller closes
 * it), ENOENT for a member the tarball has not, EIO for one of another
 * size, or the error of opening it.
 */
int
payload_open(
	const struct payload *payload,
	const char *name,
	uint64_t expected_bytes,
	int *fd)
{
	const struct payload_member *member;
	uint64_t size;
	off_t placed;
	int descriptor;
	int error;

	/* The member, of the expected size. */
	error = payload_find(payload, name, &member);
	if (error != 0)
		return error;
	if (member->bytes != expected_bytes)
		return EIO;

	/* The tarball, root's alone and of the size it was read at. */
	error = payload_file(PAYLOAD_TARBALL, &descriptor, &size);
	if (error != 0)
		return error;
	if (size != payload->size) {
		close(descriptor);
		return EIO;
	}

	/* At the member's first byte. */
	placed = lseek(descriptor, (off_t)member->offset, SEEK_SET);
	if (placed != (off_t)member->offset) {
		close(descriptor);
		return EIO;
	}

	/* Succeeded: the caller reads the member's bytes. */
	*fd = descriptor;
	return 0;
}

/*
 * Lists the published releases that have the system for this computer,
 * newest first, on output, a line each, then "end":
 *
 *   RELEASE TAG DATE BYTES    a release: its tag, its publication date
 *                             (ISO 8601, "-" when not given) and the size
 *                             of its system
 *   end                       the list's end (no RELEASE line: none)
 *   FAIL STEP ERRNO TEXT      the description could not be had
 *
 * The input's end (or any line on it) stops it.  Returns 0 or the error
 * reported.
 */
int
payload_releases(
	FILE *output,
	int input)
{
	struct release_config config;
	struct release_asset *list;
	unsigned count;
	unsigned i;
	size_t length;
	char *text;
	int error;

	/* The settings. */
	error = payload_settings(&config);
	if (error != 0) {
		payload_fail(output, "release", error, "the settings of the release cannot be read");
		return error;
	}

	/* The description, then the releases that fit. */
	error = payload_describe(output, input, &config, &text, &length);
	if (error != 0)
		return error;
	list = calloc(RELEASE_LIST_MAX, sizeof(*list));
	if (list == NULL) {
		free(text);
		payload_fail(output, "release", ENOMEM, "no memory for the releases");
		return ENOMEM;
	}
	error = release_list(text, length, &config, list, RELEASE_LIST_MAX, &count);
	free(text);
	if (error != 0) {
		free(list);
		payload_fail(output, "release", error, "the release's description is not understood");
		return error;
	}

	/* Each release, then the end. */
	for (i = 0; i < count; i++)
		fprintf(output, "RELEASE %s %s %llu\n", list[i].tag, list[i].published[0] != '\0' ? list[i].published : "-",
			(unsigned long long)list[i].size);
	fprintf(output, "end\n");
	fflush(output);
	free(list);
	syslog(LOG_INFO, "releases: %u offered", count);
	return 0;
}

/*
 * Downloads the system of the release of a tag: asks where the release
 * is, fetches its asset to the helper's directory while taking its
 * SHA-256, checks the size and the SHA-256 against the release's, and
 * only then keeps it as the tarball and copies its index for the
 * installer.  Reports on output, a line each:
 *
 *   STEP N 3 NAME                 a step begins (release, download, check)
 *   BYTES DONE TOTAL              the bytes downloaded so far
 *   DONE                          the system is downloaded and checked
 *   FAIL STEP ERRNO TEXT          it failed at a step
 *
 * The input's end (or any line on it) stops it.  Returns 0 or the error
 * reported.
 */
int
payload_download(
	FILE *output,
	int input,
	const char *tag)
{
	struct release_config config;
	struct release_asset asset;
	size_t length;
	char *text;
	int error;

	/* Nothing of an earlier download is kept. */
	(void)unlink(PAYLOAD_TARBALL);
	(void)unlink(PAYLOAD_PARTIAL);
	(void)unlink(PAYLOAD_INDEX);
	(void)unlink(PAYLOAD_INDEX_NEW);
	payload_forget_licenses();

	/* The settings. */
	fprintf(output, "STEP 1 3 release\n");
	fflush(output);
	error = payload_settings(&config);
	if (error != 0) {
		payload_fail(output, "release", error, "the settings of the release cannot be read");
		return error;
	}

	/* The release of the tag and its asset. */
	error = payload_describe(output, input, &config, &text, &length);
	if (error != 0)
		return error;
	error = release_choose(text, length, &config, tag, &asset);
	free(text);
	if (error == ENOENT) {
		payload_fail(output, "release", error, "the release is not offered");
		return error;
	}
	if (error != 0) {
		payload_fail(output, "release", error, "the release's description is not understood");
		return error;
	}
	syslog(LOG_NOTICE, "download %s of %s, %llu bytes", asset.name, asset.tag, (unsigned long long)asset.size);

	/* The asset, checked. */
	fprintf(output, "STEP 2 3 download\n");
	fflush(output);
	error = payload_fetch(output, input, &config, &asset);
	if (error != 0) {
		(void)unlink(PAYLOAD_PARTIAL);
		return error;
	}

	/* Kept, its members read and the index copied for the installer. */
	fprintf(output, "STEP 3 3 check\n");
	fflush(output);
	error = payload_keep();
	if (error != 0) {
		(void)unlink(PAYLOAD_TARBALL);
		(void)unlink(PAYLOAD_INDEX_NEW);
		payload_forget_licenses();
		payload_fail(output, "check", error, "the downloaded system is not an installer's system");
		return error;
	}

	/* Succeeded: the system is downloaded and checked. */
	syslog(LOG_NOTICE, "downloaded and checked %s of %s", asset.name, asset.tag);
	fprintf(output, "DONE\n");
	fflush(output);
	return 0;
}

/*
 * Opens a file of the helper's: a regular file of root's that no group or
 * other may write, never through a link.  Returns 0 with its descriptor
 * and size, or an errno value (EPERM for another kind of file).
 */
static int
payload_file(
	const char *path,
	int *fd,
	uint64_t *size)
{
	struct stat status;
	int descriptor;
	int regular;
	int error;

	/* The file, not a link. */
	descriptor = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (descriptor < 0)
		return errno;

	/* Root's alone. */
	error = fstat(descriptor, &status);
	if (error != 0) {
		error = errno;
		close(descriptor);
		return error;
	}
	regular = S_ISREG(status.st_mode);
	if (!regular || status.st_uid != (uid_t)(PAYLOAD_OWNER) || (status.st_mode & 022) != 0) {
		close(descriptor);
		return EPERM;
	}

	/* Succeeded: the caller reads and closes it. */
	*fd = descriptor;
	*size = (uint64_t)status.st_size;
	return 0;
}

/*
 * Reads a ustar header: the member's name (prefix and name), its size,
 * and whether it is a regular file (else a directory).  EINVAL for a
 * header whose checksum, magic, type or name is not one the helper takes.
 */
static int
payload_header(
	const uint8_t *header,
	char *name,
	uint64_t *bytes,
	int *regular)
{
	uint64_t recorded;
	uint64_t sum;
	size_t prefix;
	size_t length;
	unsigned i;
	int differs;
	int error;

	/* The checksum, counting its own field as spaces. */
	error = payload_octal(header + PAYLOAD_CHECKSUM, PAYLOAD_CHECKSUM_SIZE, &recorded);
	if (error != 0)
		return error;
	sum = 0;
	for (i = 0; i < PAYLOAD_BLOCK; i++) {
		if (i >= PAYLOAD_CHECKSUM && i < PAYLOAD_CHECKSUM + PAYLOAD_CHECKSUM_SIZE)
			sum += (uint64_t)' ';
		else
			sum += header[i];
	}
	if (sum != recorded)
		return EINVAL;

	/* The ustar magic. */
	differs = memcmp(header + PAYLOAD_MAGIC, "ustar", 5U);
	if (differs != 0)
		return EINVAL;

	/* A regular file or a directory. */
	*regular = 0;
	if (header[PAYLOAD_TYPE] == '0' || header[PAYLOAD_TYPE] == '\0')
		*regular = 1;
	else if (header[PAYLOAD_TYPE] != '5')
		return EINVAL;

	/* The size. */
	error = payload_octal(header + PAYLOAD_SIZE, PAYLOAD_SIZE_SIZE, bytes);
	if (error != 0)
		return error;

	/* The name: the prefix, a slash and the name, each without its padding. */
	prefix = strnlen((const char *)header + PAYLOAD_PREFIX, PAYLOAD_PREFIX_SIZE);
	length = strnlen((const char *)header + PAYLOAD_NAME, PAYLOAD_NAME_SIZE);
	name[0] = '\0';
	if (prefix != 0U) {
		(void)memcpy(name, header + PAYLOAD_PREFIX, prefix);
		name[prefix] = '/';
		prefix++;
	}
	(void)memcpy(name + prefix, header + PAYLOAD_NAME, length);
	name[prefix + length] = '\0';

	/* A directory's trailing slash is not part of its name. */
	length = strlen(name);
	if (!*regular && length > 1U && name[length - 1U] == '/')
		name[length - 1U] = '\0';

	/* A leading "./" is not either. */
	differs = strncmp(name, "./", 2U);
	if (differs == 0)
		(void)memmove(name, name + 2, strlen(name + 2) + 1U);

	/* The name must be one the helper takes (the root itself, ".", is a directory it passes). */
	differs = strcmp(name, ".");
	if (differs == 0 && !*regular)
		return 0;
	error = payload_name_valid(name);
	if (error != 0)
		return error;

	/* Succeeded: the header is read. */
	return 0;
}

/* Reads an octal field of a header, ended by a NUL or a space. */
static int
payload_octal(
	const uint8_t *field,
	size_t size,
	uint64_t *value)
{
	size_t i;
	int digits;

	/* Leading spaces, then the digits to a NUL or a space. */
	*value = 0;
	digits = 0;
	i = 0;
	while (i < size && field[i] == ' ')
		i++;
	for (; i < size; i++) {
		if (field[i] == '\0' || field[i] == ' ')
			break;
		if (field[i] < '0' || field[i] > '7')
			return EINVAL;
		if (*value > (UINT64_MAX >> 3))
			return EINVAL;
		*value = (*value << 3) | (uint64_t)(field[i] - '0');
		digits = 1;
	}

	/* A field without a digit is not a number. */
	if (!digits)
		return EINVAL;

	/* Succeeded: the value is read. */
	return 0;
}

/* Reports whether a block is all zeros. */
static int
payload_zero(
	const uint8_t *block)
{
	unsigned i;

	/* Any byte that is not zero. */
	for (i = 0; i < PAYLOAD_BLOCK; i++) {
		if (block[i] != 0U)
			return 0;
	}

	/* All zeros. */
	return 1;
}

/*
 * Reports whether a member's name may be taken (0, or EINVAL): relative,
 * not empty, without empty, "." or ".." components, and without control
 * characters.
 */
static int
payload_name_valid(
	const char *name)
{
	size_t start;
	size_t length;
	size_t i;

	/* Not empty, not from the root. */
	if (name[0] == '\0' || name[0] == '/')
		return EINVAL;

	/* No control character. */
	for (i = 0; name[i] != '\0'; i++) {
		if ((unsigned char)name[i] < 0x20U || (unsigned char)name[i] == 0x7fU)
			return EINVAL;
	}

	/* Each component: not empty, not "." and not "..". */
	start = 0;
	for (i = 0;; i++) {
		if (name[i] != '/' && name[i] != '\0')
			continue;
		length = i - start;
		if (length == 0U)
			return EINVAL;
		if (length == 1U && name[start] == '.')
			return EINVAL;
		if (length == 2U && name[start] == '.' && name[start + 1U] == '.')
			return EINVAL;
		if (name[i] == '\0')
			break;
		start = i + 1U;
	}

	/* The name may be taken. */
	return 0;
}

/*
 * Reads the settings of the release from RELEASE_CONFIG over the
 * defaults: a file of root's that no group or other may write.  Without
 * the file the defaults hold.  Returns 0 or an errno value.
 */
static int
payload_settings(
	struct release_config *config)
{
	char text[RELEASE_CONFIG_MAX + 1U];
	uint64_t size;
	ssize_t count;
	int fd;
	int error;

	/* The defaults, and the file when there is one. */
	release_config_default(config);
	error = payload_file(RELEASE_CONFIG, &fd, &size);
	if (error == ENOENT)
		return 0;
	if (error != 0)
		return error;
	if (size > RELEASE_CONFIG_MAX) {
		close(fd);
		return EFBIG;
	}

	/* The file, whole, read over the defaults. */
	count = pread(fd, text, (size_t)size, 0);
	close(fd);
	if (count != (ssize_t)size)
		return EIO;
	error = release_config_parse(text, (size_t)size, config);
	if (error != 0)
		return error;

	/* Succeeded: the settings are read. */
	syslog(LOG_INFO, "release settings from %s: api %s, asset %s", RELEASE_CONFIG, config->api, config->asset);
	return 0;
}

/*
 * Starts a program with an empty environment, its input /dev/null and its
 * output a pipe whose reading end is given back; its error is the
 * helper's.  Returns 0 or an errno value.
 */
static int
payload_spawn(
	char *const argument[],
	int *output,
	pid_t *child)
{
	static char *const environment[1] = { NULL };
	int pipe_ends[2];
	int null_fd;
	pid_t pid;
	int error;

	/* The pipe of the output. */
	error = pipe2(pipe_ends, O_CLOEXEC);
	if (error != 0)
		return errno;

	/* The child, which becomes the program. */
	pid = fork();
	if (pid < 0) {
		error = errno;
		close(pipe_ends[0]);
		close(pipe_ends[1]);
		return error;
	}
	if (pid == 0) {
		null_fd = open("/dev/null", O_RDONLY);
		if (null_fd >= 0)
			(void)dup2(null_fd, 0);
		(void)dup2(pipe_ends[1], 1);
		(void)signal(SIGPIPE, SIG_DFL);
		(void)execve(argument[0], argument, environment);
		_exit(127);
	}

	/* The helper keeps the reading end. */
	close(pipe_ends[1]);
	*output = pipe_ends[0];
	*child = pid;
	return 0;
}

/*
 * Waits for a program's end, stopping it first when asked.  Returns its
 * exit status, or -1 when a signal ended it.
 */
static int
payload_finish(
	pid_t child,
	int stop)
{
	pid_t ended;
	int raw;
	int exited;

	/* Stopped first when the helper gives up on it. */
	if (stop)
		(void)kill(child, SIGKILL);

	/* Its end. */
	do {
		ended = waitpid(child, &raw, 0);
	} while (ended < 0 && errno == EINTR);
	if (ended != child)
		return -1;
	exited = WIFEXITED(raw);
	if (!exited)
		return -1;

	/* How it ended. */
	return WEXITSTATUS(raw);
}

/*
 * Looks at the input without waiting: returns PAYLOAD_CANCELED when it
 * ended or brought anything (the installer stops the download), else 0.
 */
static int
payload_input(
	int input)
{
	struct pollfd watched;
	char byte;
	ssize_t count;
	int ready;

	/* Something to read, or the end. */
	watched.fd = input;
	watched.events = POLLIN;
	watched.revents = 0;
	ready = poll(&watched, 1, 0);
	if (ready <= 0)
		return 0;
	count = read(input, &byte, 1U);
	if (count < 0 && (errno == EINTR || errno == EAGAIN))
		return 0;

	/* The end of the input, or a word on it. */
	return PAYLOAD_CANCELED;
}

/*
 * Asks the API for the releases' description: its text (which the caller
 * frees) and length.  Reports its own failure.
 */
static int
payload_describe(
	FILE *output,
	int input,
	const struct release_config *config,
	char **description,
	size_t *length)
{
	struct pollfd watched[2];
	char *argument[8];
	char *text;
	uint64_t started;
	size_t used;
	ssize_t count;
	unsigned slot;
	pid_t child;
	int stream;
	int status;
	int stop;
	int error;

	/* The room of the description. */
	text = malloc(RELEASE_TEXT_MAX);
	if (text == NULL) {
		payload_fail(output, "release", ENOMEM, "no memory for the release's description");
		return ENOMEM;
	}

	/* fetch, quiet, to its output, with the settings' CA file when there is one. */
	slot = 0;
	argument[slot++] = (char *)PAYLOAD_FETCH;
	argument[slot++] = (char *)"-q";
	argument[slot++] = (char *)"-o";
	argument[slot++] = (char *)"-";
	if (config->ca_file[0] != '\0') {
		argument[slot++] = (char *)"-C";
		argument[slot++] = (char *)config->ca_file;
	}
	argument[slot++] = (char *)config->api;
	argument[slot] = NULL;
	error = payload_spawn(argument, &stream, &child);
	if (error != 0) {
		free(text);
		payload_fail(output, "release", error, "fetch cannot be started");
		return error;
	}

	/* Its output, to its end, within the time and the room. */
	used = 0;
	stop = 0;
	error = 0;
	started = payload_seconds();
	for (;;) {
		/* The output and the installer's input. */
		watched[0].fd = stream;
		watched[0].events = POLLIN;
		watched[0].revents = 0;
		watched[1].fd = input;
		watched[1].events = POLLIN;
		watched[1].revents = 0;
		(void)poll(watched, 2, 1000);

		/* The installer stops it. */
		if (watched[1].revents != 0) {
			error = payload_input(input);
			if (error == PAYLOAD_CANCELED) {
				error = ECANCELED;
				stop = 1;
				break;
			}
		}

		/* Too long. */
		if (payload_seconds() - started > PAYLOAD_API_SECONDS) {
			error = ETIMEDOUT;
			stop = 1;
			break;
		}

		/* Nothing yet. */
		if (watched[0].revents == 0)
			continue;
		count = read(stream, text + used, RELEASE_TEXT_MAX - used);
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0) {
			error = errno;
			stop = 1;
			break;
		}
		if (count == 0)
			break;
		used += (size_t)count;

		/* A description larger than the room is not one the helper takes. */
		if (used == RELEASE_TEXT_MAX) {
			error = EFBIG;
			stop = 1;
			break;
		}
	}

	/* fetch's end, which must be a success. */
	close(stream);
	status = payload_finish(child, stop);
	if (error == 0 && status != 0)
		error = EIO;
	if (error != 0) {
		free(text);
		if (error == ECANCELED)
			payload_fail(output, "release", error, "canceled");
		else
			payload_fail(output, "release", error, "the release's description could not be downloaded");
		return error;
	}

	/* Succeeded: the caller reads the description. */
	*description = text;
	*length = used;
	return 0;
}

/*
 * Downloads the asset to PAYLOAD_PARTIAL while taking its SHA-256, and
 * checks its size and SHA-256.  Reports its own failure.
 */
static int
payload_fetch(
	FILE *output,
	int input,
	const struct release_config *config,
	const struct release_asset *asset)
{
	struct command_sha256_context context;
	struct pollfd watched[2];
	uint8_t digest[32];
	uint8_t *buffer;
	char *argument[8];
	uint64_t last_byte;
	uint64_t reported;
	uint64_t done;
	ssize_t written;
	ssize_t count;
	unsigned slot;
	pid_t child;
	int difference;
	int stream;
	int status;
	int stop;
	int file;
	int error;

	/* The file, new, root's alone. */
	buffer = malloc(PAYLOAD_CHUNK);
	if (buffer == NULL) {
		payload_fail(output, "download", ENOMEM, "no memory to download with");
		return ENOMEM;
	}
	file = open(PAYLOAD_PARTIAL, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (file < 0) {
		error = errno;
		free(buffer);
		payload_fail(output, "download", error, "the download cannot be written");
		return error;
	}

	/* fetch, quiet, to its output. */
	slot = 0;
	argument[slot++] = (char *)PAYLOAD_FETCH;
	argument[slot++] = (char *)"-q";
	argument[slot++] = (char *)"-o";
	argument[slot++] = (char *)"-";
	if (config->ca_file[0] != '\0') {
		argument[slot++] = (char *)"-C";
		argument[slot++] = (char *)config->ca_file;
	}
	argument[slot++] = (char *)asset->url;
	argument[slot] = NULL;
	error = payload_spawn(argument, &stream, &child);
	if (error != 0) {
		close(file);
		free(buffer);
		payload_fail(output, "download", error, "fetch cannot be started");
		return error;
	}

	/* Each run of bytes to the file and the digest, until the end, a stop or a stall. */
	command_sha256_init(&context);
	done = 0;
	reported = 0;
	stop = 0;
	error = 0;
	last_byte = payload_seconds();
	fprintf(output, "BYTES 0 %llu\n", (unsigned long long)asset->size);
	fflush(output);
	for (;;) {
		/* The output and the installer's input. */
		watched[0].fd = stream;
		watched[0].events = POLLIN;
		watched[0].revents = 0;
		watched[1].fd = input;
		watched[1].events = POLLIN;
		watched[1].revents = 0;
		(void)poll(watched, 2, 1000);

		/* The installer stops it. */
		if (watched[1].revents != 0) {
			error = payload_input(input);
			if (error == PAYLOAD_CANCELED) {
				error = ECANCELED;
				stop = 1;
				break;
			}
		}

		/* No byte for too long. */
		if (watched[0].revents == 0) {
			if (payload_seconds() - last_byte > PAYLOAD_STALL_SECONDS) {
				error = ETIMEDOUT;
				stop = 1;
				break;
			}
			continue;
		}

		/* The bytes that came. */
		count = read(stream, buffer, PAYLOAD_CHUNK);
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0) {
			error = errno;
			stop = 1;
			break;
		}
		if (count == 0)
			break;
		last_byte = payload_seconds();

		/* More than the release's size is not the release's asset. */
		if ((uint64_t)count > asset->size - done) {
			error = EFBIG;
			stop = 1;
			break;
		}

		/* To the digest and the file. */
		(void)command_sha256_update(&context, buffer, (size_t)count);
		written = write(file, buffer, (size_t)count);
		if (written != count) {
			error = EIO;
			if (written < 0)
				error = errno;
			stop = 1;
			break;
		}
		done += (uint64_t)count;

		/* The bytes so far, now and then. */
		if (done - reported >= PAYLOAD_REPORT_BYTES || done == asset->size) {
			fprintf(output, "BYTES %llu %llu\n", (unsigned long long)done, (unsigned long long)asset->size);
			fflush(output);
			reported = done;
		}
	}

	/* fetch's end, which must be a success, and the file written out. */
	close(stream);
	free(buffer);
	status = payload_finish(child, stop);
	if (error == 0 && status != 0)
		error = EIO;
	if (error == 0) {
		error = fsync(file);
		if (error != 0)
			error = errno;
	}
	close(file);
	if (error != 0) {
		if (error == ECANCELED)
			payload_fail(output, "download", error, "canceled");
		else if (error == EFBIG)
			payload_fail(output, "download", error, "the download is larger than the release's system");
		else
			payload_fail(output, "download", error, "the system could not be downloaded");
		return error;
	}

	/* The whole, of the release's size and SHA-256. */
	command_sha256_final(&context, digest);
	if (done != asset->size) {
		payload_fail(output, "download", EIO, "the download is shorter than the release's system");
		return EIO;
	}
	difference = memcmp(digest, asset->sha256, sizeof(digest));
	if (difference != 0) {
		syslog(LOG_WARNING, "download of %s does not match the release's SHA-256", asset->name);
		payload_fail(output, "download", EBADMSG, "the download does not match the release's SHA-256");
		return EBADMSG;
	}

	/* Succeeded: the download is the release's asset. */
	return 0;
}

/*
 * Keeps the checked download as the tarball, reads its members and its
 * index, and copies the index for the installer.
 */
static int
payload_keep(void)
{
	static struct payload payload;
	static struct installer_index index;
	char *text;
	size_t length;
	ssize_t written;
	int fd;
	int error;

	/* The download, checked, is the tarball. */
	error = rename(PAYLOAD_PARTIAL, PAYLOAD_TARBALL);
	if (error != 0)
		return errno;

	/* Its members and index. */
	text = malloc(INDEX_BYTES_MAX);
	if (text == NULL)
		return ENOMEM;
	error = payload_load(&payload, &index, text, INDEX_BYTES_MAX);
	if (error != 0) {
		free(text);
		return error;
	}

	/* The licences it names, for the installer to show. */
	error = payload_licenses(&payload, &index, text);
	if (error != 0) {
		free(text);
		return error;
	}

	/* The installer's copy, written whole under a new name, then renamed. */
	length = strlen(text);
	fd = open(PAYLOAD_INDEX_NEW, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0) {
		error = errno;
		free(text);
		return error;
	}
	written = write(fd, text, length);
	free(text);
	error = 0;
	if (written != (ssize_t)length)
		error = EIO;
	if (error == 0 && fsync(fd) != 0)
		error = errno;
	close(fd);
	if (error != 0)
		return error;
	error = rename(PAYLOAD_INDEX_NEW, PAYLOAD_INDEX);
	if (error != 0)
		return errno;

	/* Succeeded: the system is kept. */
	return 0;
}

/*
 * Takes the licences the index names out of their units into the
 * licences' directory, a file each, named by the record's place.  A
 * licence's path is absolute in the index and must be plain; its unit's
 * entry must be a regular file.  Returns 0, EFBIG for more licences than
 * the installer shows, EINVAL for a record or a path that is not one,
 * ENOENT for a unit or an entry the system has not, or the error of
 * reading or writing.
 */
static int
payload_licenses(
	const struct payload *payload,
	const struct installer_index *index,
	const char *text)
{
	static struct payload_license license[PAYLOAD_LICENSES_MAX];
	unsigned count;
	unsigned unit;
	unsigned i;
	int directory;
	int wanted;
	int made;
	int error;

	/* The records, checked before anything is read. */
	error = payload_license_records(index, text, license, &count);
	if (error != 0)
		return error;

	/* The directory, root's alone, never through a link. */
	made = mkdir(PAYLOAD_LICENSES, 0700);
	if (made != 0 && errno != EEXIST)
		return errno;
	directory = open(PAYLOAD_LICENSES, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (directory < 0)
		return errno;

	/* Each unit that has a licence, inflated once. */
	error = 0;
	for (unit = 0; unit < index->unit_count && error == 0; unit++) {
		wanted = 0;
		for (i = 0; i < count; i++) {
			if (license[i].unit == unit)
				wanted = 1;
		}
		if (wanted)
			error = payload_license_unit(payload, index, unit, license, count, directory);
	}
	close(directory);
	if (error != 0)
		return error;

	/* Succeeded: the licences are there. */
	return 0;
}

/*
 * Reads the index's licence records (license UNIT PACKAGE PATH): the
 * unit's place and the path without its leading slash.  Returns 0 with
 * the count, EFBIG for more than the most, or EINVAL for a record with
 * other fields, an unknown unit or a path that is not absolute.
 */
static int
payload_license_records(
	const struct installer_index *index,
	const char *text,
	struct payload_license *license,
	unsigned *count)
{
	const char *field[PAYLOAD_RECORD_FIELDS];
	size_t field_length[PAYLOAD_RECORD_FIELDS];
	char unit_name[INDEX_NAME_MAX];
	const char *line;
	const char *end;
	const char *at;
	const char *tab;
	unsigned fields;
	unsigned slot;
	int differs;
	int error;

	/* Each line of the index. */
	*count = 0;
	line = text;
	while (*line != '\0') {
		end = strchr(line, '\n');
		if (end == NULL)
			end = line + strlen(line);

		/* Its fields, apart by a TAB. */
		fields = 0;
		at = line;
		while (fields < PAYLOAD_RECORD_FIELDS) {
			tab = memchr(at, '\t', (size_t)(end - at));
			if (tab == NULL)
				tab = end;
			field[fields] = at;
			field_length[fields] = (size_t)(tab - at);
			fields++;
			if (tab == end)
				break;
			at = tab + 1;
		}

		/* A licence record, of its four fields. */
		differs = 1;
		if (field_length[0] == 7U)
			differs = memcmp(field[0], "license", 7U);
		if (differs == 0) {
			if (fields != 4U)
				return EINVAL;
			if (*count == PAYLOAD_LICENSES_MAX)
				return EFBIG;

			/* Its unit, which the index must have. */
			if (field_length[1] == 0U || field_length[1] >= sizeof(unit_name))
				return EINVAL;
			memcpy(unit_name, field[1], field_length[1]);
			unit_name[field_length[1]] = '\0';
			error = index_find_unit(index, unit_name, &slot);
			if (error != 0)
				return EINVAL;

			/* Its path: absolute in the index, kept without the slash (unpack_tar_member checks the rest). */
			if (field_length[3] < 2U || field[3][0] != '/' || field_length[3] > sizeof(license[*count].path))
				return EINVAL;
			memcpy(license[*count].path, field[3] + 1, field_length[3] - 1U);
			license[*count].path[field_length[3] - 1U] = '\0';
			license[*count].unit = slot;
			(*count)++;
		}

		/* The next line. */
		if (*end == '\0')
			break;
		line = end + 1;
	}

	/* Succeeded: the records are read. */
	return 0;
}

/*
 * Reads one unit whole from the tarball, checks it against the index's
 * SHA-256, inflates it and writes each licence of it.  Returns 0 or the
 * error of one of those.
 */
static int
payload_license_unit(
	const struct payload *payload,
	const struct installer_index *index,
	unsigned unit,
	const struct payload_license *license,
	unsigned count,
	int directory)
{
	const struct index_unit *record;
	char name[PAYLOAD_NAME_MAX];
	const uint8_t *member;
	uint8_t *data;
	uint8_t *archive;
	uint64_t limit;
	uint64_t bytes;
	size_t length;
	size_t archive_length;
	unsigned i;
	int fd;
	int error;

	/* The unit's tarball, read whole and checked. */
	record = &index->unit[unit];
	(void)snprintf(name, sizeof(name), "units/%s.tar.gz", record->name);
	error = payload_open(payload, name, record->tar_bytes, &fd);
	if (error != 0)
		return error;
	error = unpack_read_whole(fd, record->tar_bytes, record->sha256, &data, &length);
	close(fd);
	if (error != 0)
		return error;

	/* Inflated: no more than its files, a header and padding each, and the end. */
	limit = record->bytes + record->entries * PAYLOAD_ENTRY_ROOM + PAYLOAD_ARCHIVE_SLACK;
	error = unpack_gunzip(data, length, limit, &archive, &archive_length);
	free(data);
	if (error != 0)
		return error;

	/* Each licence of the unit, a regular file of it. */
	for (i = 0; i < count && error == 0; i++) {
		if (license[i].unit != unit)
			continue;
		error = unpack_tar_member(archive, archive_length, license[i].path, &member, &bytes);
		if (error == 0)
			error = payload_license_write(directory, i, member, bytes);
	}
	free(archive);
	if (error != 0)
		return error;

	/* Succeeded: the unit's licences are written. */
	return 0;
}

/* Writes one licence under a new name, then renames it to its number. */
static int
payload_license_write(
	int directory,
	unsigned number,
	const uint8_t *data,
	uint64_t bytes)
{
	char name[16];
	char temporary[24];
	ssize_t written;
	int fd;
	int renamed;
	int error;

	/* Its names, and nothing of an earlier one. */
	(void)snprintf(name, sizeof(name), "%u", number);
	(void)snprintf(temporary, sizeof(temporary), "%u.new", number);
	(void)unlinkat(directory, temporary, 0);

	/* Its bytes, whole. */
	fd = openat(directory, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0)
		return errno;
	error = 0;
	written = 0;
	if (bytes != 0U)
		written = write(fd, data, (size_t)bytes);
	if (written < 0 || (uint64_t)written != bytes)
		error = EIO;
	close(fd);
	if (error != 0) {
		(void)unlinkat(directory, temporary, 0);
		return error;
	}

	/* In its place. */
	renamed = renameat(directory, temporary, directory, name);
	if (renamed != 0) {
		error = errno;
		(void)unlinkat(directory, temporary, 0);
		return error;
	}

	/* Succeeded: the licence is there. */
	return 0;
}

/* Removes the licences of an earlier download (the directory stays). */
static void
payload_forget_licenses(void)
{
	char name[24];
	unsigned i;
	int directory;

	/* The directory, when there is one. */
	directory = open(PAYLOAD_LICENSES, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (directory < 0)
		return;

	/* Each licence and its unfinished copy. */
	for (i = 0; i < PAYLOAD_LICENSES_MAX; i++) {
		(void)snprintf(name, sizeof(name), "%u", i);
		(void)unlinkat(directory, name, 0);
		(void)snprintf(name, sizeof(name), "%u.new", i);
		(void)unlinkat(directory, name, 0);
	}
	close(directory);
}

/* Reports a failure at a step: its errno value and a short text. */
static void
payload_fail(
	FILE *output,
	const char *step,
	int error,
	const char *text)
{
	/* The line the installer reads, and the log. */
	fprintf(output, "FAIL %s %d %s\n", step, error, text);
	fflush(output);
	syslog(LOG_NOTICE, "download failed at %s: %s (%d)", step, text, error);
}

/* Gives the seconds of the monotonic clock. */
static uint64_t
payload_seconds(void)
{
	struct timespec now;

	/* The clock that does not go back. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec;
}
