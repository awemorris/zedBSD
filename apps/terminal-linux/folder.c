/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * mikuterm's folder on Linux, and the files mikuterm reads and writes in
 * it.  The folder is the user's: $XDG_CONFIG_HOME/mikuterm, or
 * ~/.config/mikuterm when the variable is not set, holding what the
 * Windows program keeps beside itself (config/, ssh/, fonts/).  The
 * program's own files (share/) are in share/mikuterm beside the bin
 * directory the program is in, found from the program's own path.
 */

#include "mikuterm.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The folder's name under the user's configuration directory. */
#define FOLDER_NAME		"mikuterm"

/* The program's files, from the directory the program is in. */
#define FOLDER_SHARE		"/../share/mikuterm"

/* The part of a name inside the folder that is the program's files. */
#define FOLDER_SHARE_PREFIX	"share/"

/* How the folder and the directories made in it may be opened: by the user alone, as ssh keeps ~/.ssh. */
#define FOLDER_DIRECTORY_MODE	0700

/*
 * The program's path, the folder and the program's files' directory,
 * found once at the start (mikuterm_folder_init) and read only
 * afterwards, from any thread.  Empty until then.
 */
static char folder_program[MIKUTERM_PATH_MAX];
static char folder_path[MIKUTERM_PATH_MAX];
static char folder_share[MIKUTERM_PATH_MAX];

static int folder_home(char *path, size_t size);
static int folder_write(const char *path, int flags, mode_t mode, const void *bytes, size_t size);
static int folder_parent_make(const char *path);

/*
 * Finds the folder and the program's files.  Returns 0, ENAMETOOLONG for
 * a path longer than mikuterm keeps, ENOENT when the user has no home, or
 * EIO.
 */
int
mikuterm_folder_init(void)
{
	char home[MIKUTERM_PATH_MAX];
	const char *config;
	char *slash;
	ssize_t length;
	int written;
	int error;

	/* The program's own path. */
	length = readlink("/proc/self/exe", folder_program, sizeof(folder_program));
	if (length <= 0)
		return EIO;
	if ((size_t)length >= sizeof(folder_program))
		return ENAMETOOLONG;
	folder_program[length] = '\0';

	/* Its files, from the directory it is in. */
	slash = strrchr(folder_program, '/');
	if (slash == NULL)
		return EIO;
	written = snprintf(folder_share, sizeof(folder_share), "%.*s%s", (int)(slash - folder_program), folder_program, FOLDER_SHARE);
	if (written < 0 || (size_t)written >= sizeof(folder_share))
		return ENAMETOOLONG;

	/* The user's configuration directory (an absolute XDG_CONFIG_HOME, else ~/.config) and the folder in it. */
	config = getenv("XDG_CONFIG_HOME");
	if (config != NULL && config[0] == '/') {
		written = snprintf(folder_path, sizeof(folder_path), "%s/%s", config, FOLDER_NAME);
	} else {
		error = folder_home(home, sizeof(home));
		if (error != 0)
			return error;
		written = snprintf(folder_path, sizeof(folder_path), "%s/.config/%s", home, FOLDER_NAME);
	}
	if (written < 0 || (size_t)written >= sizeof(folder_path)) {
		folder_path[0] = '\0';
		return ENAMETOOLONG;
	}

	/* Succeeded: the folder and the program's files. */
	return 0;
}

/* Returns the folder (no slash at its end). */
const char *
mikuterm_folder(void)
{
	/* Read only after mikuterm_folder_init. */
	return folder_path;
}

/* Returns the program's own path, for starting another window of it. */
const char *
mikuterm_program(void)
{
	/* Read only after mikuterm_folder_init. */
	return folder_program;
}

/*
 * Writes the path of a file inside the folder ("config/mikuterm.conf"),
 * or of the program's own files for a name in share/
 * ("share/fonts/keiland.ttf").  Returns 0 or ENAMETOOLONG.
 */
int
mikuterm_path(
	char *path,
	size_t size,
	const char *inside)
{
	const char *base;
	int share;
	int written;

	/* The program's files for share/, the folder for anything else. */
	base = folder_path;
	share = strncmp(inside, FOLDER_SHARE_PREFIX, strlen(FOLDER_SHARE_PREFIX));
	if (share == 0) {
		base = folder_share;
		inside += strlen(FOLDER_SHARE_PREFIX);
	}

	/* The directory, a slash and the name, with the NUL. */
	written = snprintf(path, size, "%s/%s", base, inside);
	if (written < 0 || (size_t)written >= size)
		return ENAMETOOLONG;

	/* Succeeded: the path. */
	return 0;
}

/*
 * Writes the path of a file named in ssh/config (IdentityFile,
 * UserKnownHostsFile): ~/.ssh/X is the folder's ssh/X and ~/X the
 * folder's X, as on Windows, so a configuration written for OpenSSH keeps
 * its meaning inside the folder; a name without a directory, or relative,
 * is in ssh/; a path from the root is taken as it is.  Returns 0 or
 * ENAMETOOLONG.
 */
int
mikuterm_ssh_path(
	char *path,
	size_t size,
	const char *written)
{
	char inside[MIKUTERM_PATH_MAX];
	const char *directory;
	const char *rest;
	size_t length;
	int home_ssh;
	int home;
	int error;

	/* A path from the root as it is. */
	if (written[0] == '/') {
		length = strlen(written);
		if (length + 1U > size)
			return ENAMETOOLONG;
		memcpy(path, written, length + 1U);
		return 0;
	}

	/* The home's .ssh is the folder's ssh, the home is the folder, and anything else is in ssh. */
	home_ssh = strncmp(written, "~/.ssh/", 7);
	home = strncmp(written, "~/", 2);
	directory = "ssh/";
	rest = written;
	if (home_ssh == 0) {
		rest = written + 7;
	} else if (home == 0) {
		directory = "";
		rest = written + 2;
	}

	/* The name inside the folder. */
	length = (size_t)snprintf(inside, sizeof(inside), "%s%s", directory, rest);
	if (length >= sizeof(inside))
		return ENAMETOOLONG;

	/* Its path. */
	error = mikuterm_path(path, size, inside);
	if (error != 0)
		return error;

	/* Succeeded: the path in the folder. */
	return 0;
}

/*
 * Reads a whole file, at most limit bytes, into memory the caller frees;
 * a NUL follows its bytes.  Returns 0, ENOENT, EACCES, EFBIG, ENOMEM or
 * EIO.
 */
int
mikuterm_file_read(
	const char *path,
	size_t limit,
	char **text,
	size_t *size)
{
	struct stat status;
	char *bytes;
	size_t have;
	ssize_t got;
	int descriptor;
	int error;

	/* Nothing is read yet. */
	*text = NULL;
	*size = 0U;

	/* The file. */
	descriptor = open(path, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0) {
		error = errno;
		if (error == ENOENT || error == ENOTDIR)
			return ENOENT;
		if (error == EACCES || error == EPERM)
			return EACCES;
		return EIO;
	}

	/* Its length, within the limit; only a regular file is read. */
	error = fstat(descriptor, &status);
	if (error != 0 || !S_ISREG(status.st_mode)) {
		(void)close(descriptor);
		return EIO;
	}
	if (status.st_size < 0 || (unsigned long long)status.st_size > (unsigned long long)limit) {
		(void)close(descriptor);
		return EFBIG;
	}

	/* Room for it and a NUL. */
	bytes = malloc((size_t)status.st_size + 1U);
	if (bytes == NULL) {
		(void)close(descriptor);
		return ENOMEM;
	}

	/* Its bytes, until the end or the length. */
	have = 0U;
	while (have < (size_t)status.st_size) {
		got = read(descriptor, bytes + have, (size_t)status.st_size - have);
		if (got < 0 && errno == EINTR)
			continue;
		if (got < 0) {
			(void)close(descriptor);
			free(bytes);
			return EIO;
		}

		/* The end of the file ends the reading. */
		if (got == 0)
			break;
		have += (size_t)got;
	}

	/* The file closes once it is read. */
	(void)close(descriptor);

	/* Succeeded: the text, NUL-terminated. */
	bytes[have] = '\0';
	*text = bytes;
	*size = have;
	return 0;
}

/*
 * Appends bytes to a file in one write, making the file when it is not
 * there (and its directories, as ssh makes ~/.ssh): another program
 * appending to it at the same time adds its own lines whole, before or
 * after.  Returns 0, EACCES, ENOENT, ENOMEM or EIO.
 */
int
mikuterm_file_append(
	const char *path,
	const void *bytes,
	size_t size)
{
	int error;

	/* The directories, then the bytes at the file's end. */
	error = folder_parent_make(path);
	if (error != 0)
		return error;
	error = folder_write(path, O_WRONLY | O_CREAT | O_APPEND, 0644, bytes, size);
	if (error != 0)
		return error;

	/* Succeeded: appended. */
	return 0;
}

/*
 * Replaces a file with the bytes: they are written to a new file beside
 * it that then takes its name, so a reader finds the old file or the new
 * one whole.  Returns 0, EACCES, ENOENT, ENOMEM or EIO.
 */
int
mikuterm_file_replace(
	const char *path,
	const void *bytes,
	size_t size)
{
	char part[MIKUTERM_PATH_MAX + 16U];
	int written;
	int error;

	/* The new file's name beside the file. */
	written = snprintf(part, sizeof(part), "%s.new", path);
	if (written < 0 || (size_t)written >= sizeof(part))
		return ENOENT;

	/* The directories and the new file, written whole. */
	error = folder_parent_make(path);
	if (error != 0)
		return error;
	error = folder_write(part, O_WRONLY | O_CREAT | O_TRUNC, 0644, bytes, size);
	if (error != 0) {
		(void)unlink(part);
		return error;
	}

	/* The new file takes the name. */
	error = rename(part, path);
	if (error != 0) {
		error = errno;
		(void)unlink(part);
		if (error == EACCES || error == EPERM)
			return EACCES;
		return EIO;
	}

	/* Succeeded: replaced. */
	return 0;
}

/*
 * Makes a new file with the bytes, never over one that is there (EEXIST),
 * in its directories made when they are missing.  A secret file (a
 * private key) is made so that only the user may open it, as ssh wants a
 * key file kept; a file this call made and could not fill is removed.
 * Returns 0, EEXIST, ENOENT, EACCES, ENOMEM or EIO.
 */
int
mikuterm_file_create(
	const char *path,
	const void *bytes,
	size_t size,
	int secret)
{
	mode_t mode;
	int error;

	/* The file's mode: the user's alone for a secret. */
	mode = 0644;
	if (secret)
		mode = 0600;

	/* The directories, then the new file. */
	error = folder_parent_make(path);
	if (error != 0)
		return error;
	error = folder_write(path, O_WRONLY | O_CREAT | O_EXCL, mode, bytes, size);
	if (error == EEXIST)
		return EEXIST;
	if (error != 0) {
		(void)unlink(path);
		return error;
	}

	/* Succeeded: made. */
	return 0;
}

/* Tells whether a file or a directory is at a path (1) or not (0). */
int
mikuterm_file_exists(
	const char *path)
{
	struct stat status;
	int error;

	/* Anything at the path. */
	error = stat(path, &status);
	if (error != 0)
		return 0;

	/* Succeeded: something is there. */
	return 1;
}

/* Writes the user's home directory: $HOME when it is absolute, else the user's entry's. */
static int
folder_home(
	char *path,
	size_t size)
{
	const struct passwd *entry;
	const char *home;
	int written;

	/* The environment's, else the user database's. */
	home = getenv("HOME");
	if (home == NULL || home[0] != '/') {
		entry = getpwuid(getuid());
		if (entry == NULL || entry->pw_dir == NULL || entry->pw_dir[0] != '/')
			return ENOENT;
		home = entry->pw_dir;
	}

	/* The path, which must fit. */
	written = snprintf(path, size, "%s", home);
	if (written < 0 || (size_t)written >= size)
		return ENAMETOOLONG;

	/* Succeeded: the home. */
	return 0;
}

/* Opens a file with flags and a mode and writes the bytes; returns 0 or an errno value. */
static int
folder_write(
	const char *path,
	int flags,
	mode_t mode,
	const void *bytes,
	size_t size)
{
	const unsigned char *next;
	ssize_t wrote;
	int descriptor;
	int error;

	/* The file. */
	descriptor = open(path, flags | O_CLOEXEC, mode);
	if (descriptor < 0) {
		error = errno;
		if (error == EEXIST)
			return EEXIST;
		if (error == ENOENT || error == ENOTDIR)
			return ENOENT;
		if (error == EACCES || error == EPERM || error == EROFS)
			return EACCES;
		return EIO;
	}

	/* The bytes, all of them. */
	next = bytes;
	while (size != 0U) {
		wrote = write(descriptor, next, size);
		if (wrote < 0 && errno == EINTR)
			continue;
		if (wrote <= 0) {
			(void)close(descriptor);
			return EIO;
		}
		next += wrote;
		size -= (size_t)wrote;
	}

	/* The file closes; a failed close is a failed write. */
	error = close(descriptor);
	if (error != 0)
		return EIO;

	/* Succeeded: written. */
	return 0;
}

/*
 * Makes the directories a file's path is in, each missing one for the
 * user alone.  Returns 0 (also when they were there already), ENOENT,
 * EACCES or EIO.
 */
static int
folder_parent_make(
	const char *path)
{
	char directory[MIKUTERM_PATH_MAX];
	size_t length;
	size_t index;
	int error;

	/* The file's directory. */
	length = strlen(path);
	if (length >= sizeof(directory))
		return ENOENT;
	memcpy(directory, path, length + 1U);
	while (length != 0U && directory[length - 1U] != '/')
		length--;
	if (length <= 1U)
		return 0;
	directory[length - 1U] = '\0';

	/* Each directory from the root, made when it is missing. */
	for (index = 1U; ; index++) {
		/* The end of one directory's name, or of the whole path. */
		if (directory[index] != '/' && directory[index] != '\0')
			continue;

		/* That directory, made when it is not there. */
		if (directory[index] == '/') {
			directory[index] = '\0';
			error = mkdir(directory, FOLDER_DIRECTORY_MODE);
			directory[index] = '/';
		} else {
			error = mkdir(directory, FOLDER_DIRECTORY_MODE);
		}
		if (error != 0 && errno != EEXIST) {
			if (errno == EACCES || errno == EPERM || errno == EROFS)
				return EACCES;
			return ENOENT;
		}

		/* The whole path made ends the making. */
		if (directory[index] == '\0')
			break;
	}

	/* Succeeded: the directories are there. */
	return 0;
}
