/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the information card (Get Info, spec §21) shows of one file or
 * folder: its status as lstat sees it, the target of a link, its type,
 * extended attributes and the ways it can be opened, and, when asked
 * for, its SHA-256 checksum.
 *
 * The checksum of a large file takes a while, so it is computed a piece at
 * a time from the main loop (fm_info_checksum_step), within a budget of
 * time each round, as the file operations are.
 */

#include "files.h"

#include <errno.h>
#include <fcntl.h>
#include <sha2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

/* How much of the file one read of the checksum takes. */
#define INFO_CHUNK		(256U * 1024U)

/* The largest list of attribute names read. */
#define INFO_NAMES_MAX		4096

/*
 * The checksum in progress: the hash's state and the buffer the file is
 * read through.  One is allocated when the checksum starts and freed when
 * it ends.
 */
struct info_hash {
	SHA2_CTX context;
	uint8_t buffer[INFO_CHUNK];
};

static void info_attributes(struct fm_info *info);
static void info_checksum_end(struct fm_info *info, int error);
static char info_type_letter(mode_t mode);

/*
 * Gathers what the card shows of a path, forgetting what it showed before.
 *
 * Returns 0, or the errno value of a path whose status cannot be read (the
 * card then says so).
 */
int
fm_info_gather(
	struct fm_info *info,
	const char *path)
{
	struct stat status;
	struct stat followed;
	const char *name;
	ssize_t length;
	mode_t mode;
	int is_link;
	int error;

	/* Nothing of the file before. */
	fm_info_release(info);
	snprintf(info->path, sizeof(info->path), "%s", path);
	info->child_count = -1;

	/* The status of the path itself (a link as a link). */
	error = lstat(path, &status);
	if (error != 0) {
		info->error = errno;
		info->mime = fm_mime_guess(path, 0);
		return info->error;
	}

	/* What the status says. */
	info->mode = status.st_mode;
	info->uid = status.st_uid;
	info->gid = status.st_gid;
	info->size = (uint64_t)status.st_size;
	info->links = (long)status.st_nlink;
	info->modified = status.st_mtime;
	info->changed = status.st_ctime;
	info->accessed = status.st_atime;

	/* A link's target, and what the link leads to, which decides its kind. */
	mode = status.st_mode;
	is_link = S_ISLNK(status.st_mode);
	if (is_link != 0) {
		length = readlink(path, info->target, sizeof(info->target) - 1U);
		if (length >= 0)
			info->target[length] = '\0';
		error = stat(path, &followed);
		if (error == 0)
			mode = followed.st_mode;
	}

	/* A folder counts its items; a file's type comes from its name and, when that says little, its first bytes. */
	name = strrchr(path, '/');
	if (name == NULL)
		name = path;
	else
		name++;
	info->folder = S_ISDIR(mode);
	info->mime = fm_mime_guess(name, mode);
	if (info->folder != 0) {
		info->child_count = fm_dir_count(path, 1);
	} else {
		info->mime = fm_mime_sniff(path, info->mime);
	}

	/* Its extended attributes. */
	info_attributes(info);

	/* The ways a file can be opened (a folder opens in the window). */
	if (info->folder == 0)
		info->opener_count = fm_apps_for(path, info->mime, mode, info->openers, FM_OPENERS);

	/* The log line the tests wait for. */
	fm_log("INFO path=%s mode=%04o uid=%lu attributes=%d openers=%d", path, (unsigned)(info->mode & 07777), (unsigned long)info->uid, info->attribute_count, info->opener_count);

	/* Succeeded: the card can show the file. */
	return 0;
}

/*
 * Starts computing the file's checksum.
 *
 * Returns 0, EISDIR for something that is not a regular file, ENOMEM, or
 * the errno value of a file that cannot be opened.
 */
int
fm_info_checksum_start(
	struct fm_info *info)
{
	struct info_hash *hash;
	int descriptor;
	int regular;

	/* Only a regular file has one; one already started goes on. */
	if (info->checksum_state == FM_CHECKSUM_RUNNING)
		return 0;
	regular = S_ISREG(info->mode);
	if (regular == 0)
		return EISDIR;

	/* The hash's state and buffer. */
	hash = malloc(sizeof(*hash));
	if (hash == NULL)
		return ENOMEM;

	/* The file. */
	descriptor = open(info->path, O_RDONLY);
	if (descriptor < 0) {
		free(hash);
		info_checksum_end(info, errno);
		return info->checksum_error;
	}

	/* The computation starts from the file's first byte. */
	SHA256Init(&hash->context);
	info->checksum_context = hash;
	info->checksum_descriptor = descriptor;
	info->checksum_done = 0;
	info->checksum_state = FM_CHECKSUM_RUNNING;
	fm_log("CHECKSUM start path=%s", info->path);

	/* Succeeded: the main loop moves it on. */
	return 0;
}

/*
 * Moves the checksum on for about a budget of milliseconds.
 *
 * Returns nonzero when it moved (the card shows its progress or its end).
 */
int
fm_info_checksum_step(
	struct fm_info *info,
	uint64_t budget_ms)
{
	struct info_hash *hash;
	uint8_t digest[SHA256_DIGEST_LENGTH];
	uint64_t deadline;
	uint64_t now;
	ssize_t count;
	int index;

	/* Nothing runs. */
	if (info->checksum_state != FM_CHECKSUM_RUNNING)
		return 0;

	/* Pieces of the file into the hash, until the budget is spent or the file ends. */
	hash = info->checksum_context;
	deadline = fm_ops_clock() + budget_ms;
	for (;;) {
		count = read(info->checksum_descriptor, hash->buffer, sizeof(hash->buffer));
		if (count < 0) {
			info_checksum_end(info, errno);
			return 1;
		}

		/* The end of the file ends the computation. */
		if (count == 0)
			break;

		/* The piece counts. */
		SHA256Update(&hash->context, hash->buffer, (size_t)count);
		info->checksum_done += (uint64_t)count;

		/* The budget spent, the rest waits for the next round. */
		now = fm_ops_clock();
		if (now >= deadline)
			return 1;
	}

	/* The digest, written in hexadecimal. */
	SHA256Final(digest, &hash->context);
	for (index = 0; index < SHA256_DIGEST_LENGTH; index++)
		snprintf(info->checksum + 2 * index, sizeof(info->checksum) - (size_t)(2 * index), "%02x", digest[index]);

	/* The computation is done. */
	info_checksum_end(info, 0);
	fm_log("CHECKSUM done path=%s sha256=%s", info->path, info->checksum);

	/* Succeeded: the card shows the checksum. */
	return 1;
}

/*
 * Forgets the file shown, stopping a checksum in progress.
 */
void
fm_info_release(
	struct fm_info *info)
{
	/* A checksum in progress stops, its file closed and its state freed. */
	if (info->checksum_state == FM_CHECKSUM_RUNNING)
		info_checksum_end(info, ECANCELED);

	/* Nothing is shown now. */
	memset(info, 0, sizeof(*info));
	info->checksum_descriptor = -1;
}

/*
 * Writes a mode as ls writes it, with its octal value: "-rwxr-xr-x (755)";
 * the set-user-ID, set-group-ID and sticky bits show as s, s and t.
 */
void
fm_mode_text(
	mode_t mode,
	char *text,
	size_t size)
{
	char letters[11];

	/* The kind of file. */
	letters[0] = info_type_letter(mode);

	/* The owner's read, write and execute (s with set-user-ID). */
	letters[1] = '-';
	letters[2] = '-';
	letters[3] = '-';
	if ((mode & S_IRUSR) != 0)
		letters[1] = 'r';
	if ((mode & S_IWUSR) != 0)
		letters[2] = 'w';
	if ((mode & S_IXUSR) != 0)
		letters[3] = 'x';
	if ((mode & S_ISUID) != 0) {
		letters[3] = 'S';
		if ((mode & S_IXUSR) != 0)
			letters[3] = 's';
	}

	/* The group's (s with set-group-ID). */
	letters[4] = '-';
	letters[5] = '-';
	letters[6] = '-';
	if ((mode & S_IRGRP) != 0)
		letters[4] = 'r';
	if ((mode & S_IWGRP) != 0)
		letters[5] = 'w';
	if ((mode & S_IXGRP) != 0)
		letters[6] = 'x';
	if ((mode & S_ISGID) != 0) {
		letters[6] = 'S';
		if ((mode & S_IXGRP) != 0)
			letters[6] = 's';
	}

	/* Everyone else's (t with the sticky bit). */
	letters[7] = '-';
	letters[8] = '-';
	letters[9] = '-';
	if ((mode & S_IROTH) != 0)
		letters[7] = 'r';
	if ((mode & S_IWOTH) != 0)
		letters[8] = 'w';
	if ((mode & S_IXOTH) != 0)
		letters[9] = 'x';
	if ((mode & S_ISVTX) != 0) {
		letters[9] = 'T';
		if ((mode & S_IXOTH) != 0)
			letters[9] = 't';
	}

	/* The letters and the octal value. */
	letters[10] = '\0';
	snprintf(text, size, "%s (%o)", letters, (unsigned)(mode & 07777));
}

/* Reads the names of the file's extended attributes (the names only, WS131 D14). */
static void
info_attributes(
	struct fm_info *info)
{
	char *names;
	char *name;
	ssize_t length;

	/* Room for the names. */
	names = malloc(INFO_NAMES_MAX);
	if (names == NULL)
		return;

	/* The names, one after another, each ending in a NUL; a file system without them has none. */
	length = llistxattr(info->path, names, INFO_NAMES_MAX);
	if (length <= 0) {
		free(names);
		return;
	}

	/* Each name, as many as the card lists. */
	name = names;
	while (name < names + length) {
		if (info->attribute_count == FM_INFO_ATTRIBUTES) {
			info->attributes_more = 1;
			break;
		}

		/* The name. */
		snprintf(info->attributes[info->attribute_count].name, sizeof(info->attributes[0].name), "%s", name);
		info->attribute_count++;

		/* The next name. */
		name += strlen(name) + 1U;
	}

	/* The names are copied. */
	free(names);
}

/* Ends the checksum: its file closed, its state freed, and its outcome kept (0 for done). */
static void
info_checksum_end(
	struct fm_info *info,
	int error)
{
	/* The file and the hash. */
	if (info->checksum_descriptor >= 0)
		close(info->checksum_descriptor);
	info->checksum_descriptor = -1;
	free(info->checksum_context);
	info->checksum_context = NULL;

	/* Done, or failed with a reason. */
	info->checksum_error = error;
	info->checksum_state = FM_CHECKSUM_DONE;
	if (error != 0)
		info->checksum_state = FM_CHECKSUM_FAILED;
}

/* Returns the letter ls gives a kind of file. */
static char
info_type_letter(
	mode_t mode)
{
	/* Each kind's letter; anything else is a regular file. */
	switch (mode & S_IFMT) {
	case S_IFDIR:
		return 'd';
	case S_IFLNK:
		return 'l';
	case S_IFCHR:
		return 'c';
	case S_IFBLK:
		return 'b';
	case S_IFIFO:
		return 'p';
	case S_IFSOCK:
		return 's';
	default:
		break;
	}

	/* A regular file. */
	return '-';
}
