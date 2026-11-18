/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The trash of files: the home trash of the freedesktop.org
 * Trash specification, so that other desktops' programs (and later GTK and
 * Qt applications on zedBSD) share it.
 *
 * $XDG_DATA_HOME/Trash (or ~/.local/share/Trash) holds files/NAME, the
 * trashed item, and info/NAME.trashinfo, which says where it was and when
 * it was trashed:
 *
 *     [Trash Info]
 *     Path=/home/user/Documents/Report%20v2.pdf
 *     DeletionDate=2026-09-27T16:20:00
 *
 * The path is escaped as a URL's path is (bytes other than letters, digits
 * and -._~/ as %XX), the date is local time.
 *
 * An item on another file system than the home trash's goes to the trash
 * at the top of its own volume (ws127-p003, F-041), so that it is renamed
 * rather than copied: $topdir/.Trash/$uid when the administrator made a
 * sticky $topdir/.Trash, else $topdir/.Trash-$uid.  Those trashes record
 * the path relative to $topdir, so the volume may be mounted elsewhere
 * later.  When neither can be used the home trash takes the item (a copy).
 *
 * The system's own mounts get no trash of their own (ws127-p008, as GIO
 * leaves its system-internal mounts alone): the root, /tmp, /var/tmp and
 * /var, and anything under /dev, /proc, /sys, /run, /boot, /snap and
 * /var/lib.  An item there goes to the home trash.
 */

#include "ops.h"
#include "mounts.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The tops of the system's mounts that get no trash of their own (the whole path). */
static const char *const trash_system_tops[] = {
	"/", "/tmp", "/var/tmp", "/var"
};

/* The folders under which the system's mounts get no trash of their own (the folder and below). */
static const char *const trash_system_trees[] = {
	"/dev", "/proc", "/sys", "/run", "/boot", "/snap", "/var/lib"
};

/* The largest .trashinfo file read. */
#define TRASH_INFO_MAX		8192

static int trash_mkdir(const char *path);
static void trash_mkdir_parents(const char *path);
static int trash_hex(int character);
static void trash_escape(const char *path, char *escaped, size_t size);
static int trash_unescape(const char *escaped, char *path, size_t size);
static int trash_make_folders(const char *trash);
static void trash_parent(const char *path, char *parent, size_t size);
static const char *trash_base(const char *path);
static int trash_topdir(const char *trash, char *topdir, size_t size);
static int trash_mount_top(const char *item, char *topdir, size_t size);
static int trash_volume(const char *topdir, char *trash, size_t size);
static int trash_usable(const char *path, int shared);
static int trash_list_add(char ***trashes, size_t *count, const char *path);
static void trash_list_free(char **trashes, size_t count);
static int trash_list_volume(char ***trashes, size_t *count, const char *topdir);
static int trash_list_volume_add(char ***trashes, size_t *count, const char *path);
static int trash_elsewhere(const char *item, const char *home, char *trash, size_t size);
static int trash_system_top(const char *topdir);

/*
 * Writes the trash's path (its files and info folders are made when
 * missing).
 *
 * Returns 0, or an errno value when there is no home or the folders cannot
 * be made.
 */
int
fm_trash_path(
	char *path,
	size_t size)
{
	char folder[FM_OPS_PATH_MAX];
	const char *data;
	const char *home;
	int error;

	/* $XDG_DATA_HOME/Trash, or ~/.local/share/Trash. */
	data = getenv("XDG_DATA_HOME");
	home = getenv("HOME");
	if (data != NULL && data[0] == '/') {
		trash_mkdir_parents(data);
		snprintf(path, size, "%s/Trash", data);
	} else if (home != NULL && home[0] != '\0') {
		snprintf(folder, sizeof(folder), "%s/.local", home);
		(void)trash_mkdir(folder);
		snprintf(folder, sizeof(folder), "%s/.local/share", home);
		(void)trash_mkdir(folder);
		snprintf(path, size, "%s/.local/share/Trash", home);
	} else {
		return ENOENT;
	}

	/* The trash and its two folders. */
	error = trash_make_folders(path);
	if (error != 0)
		return error;

	/* Succeeded: the trash is there. */
	return 0;
}

/*
 * Reads where a trashed item (by its name in the trash's files) was and
 * when it was trashed.
 *
 * Returns 0, or an errno value (ENOENT when it has no record).
 */
int
fm_trash_info_read(
	const char *trash,
	const char *name,
	char *original,
	size_t size,
	time_t *deleted)
{
	struct tm moment;
	char path[FM_OPS_PATH_MAX];
	char text[TRASH_INFO_MAX + 1];
	char topdir[FM_OPS_PATH_MAX];
	char relative[FM_OPS_PATH_MAX];
	char *line;
	char *next;
	ssize_t length;
	int descriptor;
	int found;
	int match;
	int fields;
	int error;
	int root;
	int written;

	/* The record's text. */
	snprintf(path, sizeof(path), "%s/info/%s.trashinfo", trash, name);
	descriptor = open(path, O_RDONLY);
	if (descriptor < 0)
		return errno;
	length = read(descriptor, text, TRASH_INFO_MAX);
	close(descriptor);
	if (length < 0)
		return EIO;
	text[length] = '\0';

	/* Each line: the path and the date. */
	found = 0;
	*deleted = 0;
	for (line = text; line != NULL && *line != '\0'; line = next) {
		next = strchr(line, '\n');
		if (next != NULL) {
			*next = '\0';
			next++;
		}

		/* The escaped path. */
		match = strncmp(line, "Path=", 5);
		if (match == 0) {
			found = trash_unescape(line + 5, original, size);
			continue;
		}

		/* The local date and time. */
		match = strncmp(line, "DeletionDate=", 13);
		if (match == 0) {
			memset(&moment, 0, sizeof(moment));
			fields = sscanf(line + 13, "%d-%d-%dT%d:%d:%d", &moment.tm_year, &moment.tm_mon, &moment.tm_mday, &moment.tm_hour, &moment.tm_min, &moment.tm_sec);
			if (fields == 6) {
				moment.tm_year -= 1900;
				moment.tm_mon -= 1;
				moment.tm_isdst = -1;
				*deleted = mktime(&moment);
			}
		}
	}

	/* A record without a path says nothing. */
	if (found == 0)
		return EINVAL;

	/* An absolute path is where the item was. */
	if (original[0] == '/')
		return 0;

	/* A relative path is under the top of the trash's volume. */
	error = trash_topdir(trash, topdir, sizeof(topdir));
	if (error != 0)
		return EINVAL;

	/* Joins the volume's top and the relative path ("/" has no slash added). */
	snprintf(relative, sizeof(relative), "%s", original);
	root = strcmp(topdir, "/");
	if (root == 0)
		written = snprintf(original, size, "/%s", relative);
	else
		written = snprintf(original, size, "%s/%s", topdir, relative);
	if (written < 0 || (size_t)written >= size)
		return ENAMETOOLONG;

	/* Succeeded: where the item was on its volume. */
	return 0;
}

/*
 * Writes the record of a trashed item at a path: where it was and when.
 *
 * Returns 0, or an errno value.
 */
int
fm_trash_info_write(
	const char *path,
	const char *original,
	time_t deleted)
{
	struct tm *moment;
	char escaped[FM_OPS_PATH_MAX * 3];
	char text[FM_OPS_PATH_MAX * 3 + 128];
	char date[64];
	char info[FM_OPS_PATH_MAX];
	char trash[FM_OPS_PATH_MAX];
	char topdir[FM_OPS_PATH_MAX];
	const char *recorded;
	ssize_t written;
	size_t prefix;
	int descriptor;
	int length;
	int error;
	int root;
	int match;

	/* The date in local time. */
	moment = localtime(&deleted);
	if (moment == NULL)
		return EINVAL;
	snprintf(date, sizeof(date), "%04d-%02d-%02dT%02d:%02d:%02d", moment->tm_year + 1900, moment->tm_mon + 1, moment->tm_mday, moment->tm_hour, moment->tm_min, moment->tm_sec);

	/*
	 * A trash at the top of a volume records the path relative to that
	 * top, so the volume can be mounted elsewhere; the home trash records
	 * it whole.  The trash is the folder above the record's info folder.
	 */
	recorded = original;
	trash_parent(path, info, sizeof(info));
	trash_parent(info, trash, sizeof(trash));
	error = trash_topdir(trash, topdir, sizeof(topdir));
	if (error == 0) {
		/* The top's length before the slash that follows it (none for the root). */
		prefix = strlen(topdir);
		root = strcmp(topdir, "/");
		if (root == 0)
			prefix = 0;

		/* A path under the top is recorded from the part after that slash. */
		match = strncmp(original, topdir, prefix);
		if (match == 0 &&
		    original[prefix] == '/' &&
		    original[prefix + 1U] != '\0')
			recorded = original + prefix + 1U;
	}

	/* The record's text. */
	trash_escape(recorded, escaped, sizeof(escaped));
	length = snprintf(text, sizeof(text), "[Trash Info]\nPath=%s\nDeletionDate=%s\n", escaped, date);
	if (length < 0 || (size_t)length >= sizeof(text))
		return ENAMETOOLONG;

	/* A new file (the name was chosen free), written in full. */
	descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (descriptor < 0)
		return errno;
	written = write(descriptor, text, (size_t)length);
	close(descriptor);
	if (written != (ssize_t)length) {
		(void)unlink(path);
		return EIO;
	}

	/* Succeeded: the record is written. */
	return 0;
}

/*
 * Chooses a name in the trash for an item's name that neither its files
 * nor its records use: the name, then "name.2", "name.3", ...
 *
 * Returns 0, or an errno value.
 */
int
fm_trash_name(
	const char *trash,
	const char *base,
	char *name,
	size_t size)
{
	struct stat status;
	char path[FM_OPS_PATH_MAX];
	unsigned number;
	int taken;
	int written;

	/* The name, then numbered ones. */
	for (number = 1; number < 100000U; number++) {
		if (number == 1U)
			written = snprintf(name, size, "%s", base);
		else
			written = snprintf(name, size, "%s.%u", base, number);
		if (written < 0 || (size_t)written >= size)
			return ENAMETOOLONG;

		/* Taken by a file in the trash. */
		snprintf(path, sizeof(path), "%s/files/%s", trash, name);
		taken = lstat(path, &status);
		if (taken == 0)
			continue;

		/* Or by a record (a stray one counts too). */
		snprintf(path, sizeof(path), "%s/info/%s.trashinfo", trash, name);
		taken = lstat(path, &status);
		if (taken == 0)
			continue;

		/* Succeeded: this name is free. */
		return 0;
	}

	/* Every name is taken (it does not happen). */
	return EEXIST;
}

/*
 * Chooses the trash an item goes into: the home trash when the item is on
 * the home trash's file system, else the trash at the top of the item's
 * own volume, made when missing; the home trash again when that cannot be
 * used.
 *
 * Returns 0, or an errno value when not even the home trash is there.
 */
int
fm_trash_for(
	const char *item,
	char *trash,
	size_t size)
{
	char home[FM_OPS_PATH_MAX];
	int error;
	int written;

	/* The home trash, which is the answer unless the item is elsewhere. */
	error = fm_trash_path(home, sizeof(home));
	if (error != 0)
		return error;

	/* An item on another volume goes into that volume's trash when it can. */
	error = trash_elsewhere(item, home, trash, size);
	if (error == 0)
		return 0;

	/* The home trash takes it otherwise (across file systems, by a copy). */
	written = snprintf(trash, size, "%s", home);
	if (written < 0 || (size_t)written >= size)
		return ENAMETOOLONG;

	/* Succeeded: the item goes into the home trash. */
	return 0;
}

/*
 * Finds the trash an item of a trash's files folder belongs to: the home
 * trash, or a trash at the top of a volume ($topdir/.Trash/$uid or
 * $topdir/.Trash-$uid).
 *
 * Returns 0, or EINVAL when the item is not directly in a trash's files.
 */
int
fm_trash_of(
	const char *item,
	char *trash,
	size_t size)
{
	char files[FM_OPS_PATH_MAX];
	char found[FM_OPS_PATH_MAX];
	char home[FM_OPS_PATH_MAX];
	char topdir[FM_OPS_PATH_MAX];
	int match;
	int error;
	int written;

	/* The item's folder must be a folder called files. */
	trash_parent(item, files, sizeof(files));
	match = strcmp(trash_base(files), "files");
	if (match != 0)
		return EINVAL;

	/* The folder above it is the candidate trash. */
	trash_parent(files, found, sizeof(found));

	/* It is the home trash when the home trash's path is the same. */
	error = fm_trash_path(home, sizeof(home));
	match = 1;
	if (error == 0)
		match = strcmp(found, home);

	/* Any other trash must be a volume's, by its name. */
	if (match != 0) {
		error = trash_topdir(found, topdir, sizeof(topdir));
		if (error != 0)
			return EINVAL;
	}

	/* The trash's path for the caller. */
	written = snprintf(trash, size, "%s", found);
	if (written < 0 || (size_t)written >= size)
		return ENAMETOOLONG;

	/* Succeeded: the item is in this trash. */
	return 0;
}

/*
 * Lists every trash of the user: the home trash first, then the trashes
 * at the tops of the mounted volumes that are there (none is made).  The
 * table is freed with fm_paths_free.
 *
 * Returns 0, or an errno value (the table is then empty).
 */
int
fm_trash_list(
	char ***trashes,
	size_t *count)
{
	struct fm_mounts *table;
	struct fm_mount mount;
	char home[FM_OPS_PATH_MAX];
	int available;
	int error;

	/* Empty until the home trash is found. */
	*trashes = NULL;
	*count = 0;

	/* The home trash first. */
	error = fm_trash_path(home, sizeof(home));
	if (error == 0) {
		error = trash_list_add(trashes, count, home);
		if (error != 0)
			return error;
	}

	/* The mounted volumes; without their table, only the home trash. */
	error = fm_mounts_open(&table);
	if (error != 0)
		return 0;

	/* The trashes at the top of each volume that are there. */
	for (;;) {
		available = fm_mounts_next(table, &mount);
		if (available <= 0)
			break;
		error = trash_list_volume(trashes, count, mount.path);
		if (error != 0)
			break;
	}

	/* The table is let go whatever happened. */
	fm_mounts_close(table);

	/* Memory that ran out lists nothing. */
	if (error != 0) {
		trash_list_free(*trashes, *count);
		*trashes = NULL;
		*count = 0;
		return error;
	}

	/* Succeeded: the trashes are listed. */
	return 0;
}

/* Makes a folder unless it is there; returns 0 or an errno value. */
static int
trash_mkdir(
	const char *path)
{
	int status;

	/* The folder, private to the user. */
	status = mkdir(path, 0700);
	if (status != 0 && errno != EEXIST)
		return errno;

	/* Succeeded: the folder is there. */
	return 0;
}

/* Makes a folder and the folders above it that are missing (as mkdir -p). */
static void
trash_mkdir_parents(
	const char *path)
{
	char partial[FM_OPS_PATH_MAX];
	size_t index;

	/* Each prefix that ends at a slash, then the whole path. */
	snprintf(partial, sizeof(partial), "%s", path);
	for (index = 1; partial[index] != '\0'; index++) {
		if (partial[index] != '/')
			continue;
		partial[index] = '\0';
		(void)trash_mkdir(partial);
		partial[index] = '/';
	}

	/* The folder itself. */
	(void)trash_mkdir(partial);
}

/* Reports the value of a hexadecimal digit, or -1. */
static int
trash_hex(
	int character)
{
	/* The three ranges of digits. */
	if (character >= '0' && character <= '9')
		return character - '0';
	if (character >= 'a' && character <= 'f')
		return character - 'a' + 10;
	if (character >= 'A' && character <= 'F')
		return character - 'A' + 10;

	/* Not a digit. */
	return -1;
}

/* Escapes a path as a URL's path: letters, digits and -._~/ stay, other bytes become %XX. */
static void
trash_escape(
	const char *path,
	char *escaped,
	size_t size)
{
	static const char digits[] = "0123456789ABCDEF";
	const unsigned char *byte;
	size_t used;
	int plain;

	/* Each byte, while there is room for three more and the end. */
	used = 0;
	for (byte = (const unsigned char *)path; *byte != '\0' && used + 4U < size; byte++) {
		/* The bytes a URL's path keeps as they are. */
		plain = 0;
		if ((*byte >= 'a' && *byte <= 'z') || (*byte >= 'A' && *byte <= 'Z') || (*byte >= '0' && *byte <= '9'))
			plain = 1;
		if (*byte == '-' || *byte == '.' || *byte == '_' || *byte == '~' || *byte == '/')
			plain = 1;
		if (plain != 0) {
			escaped[used] = (char)*byte;
			used++;
			continue;
		}

		/* Any other as %XX. */
		escaped[used] = '%';
		escaped[used + 1U] = digits[*byte >> 4];
		escaped[used + 2U] = digits[*byte & 0x0fU];
		used += 3U;
	}

	/* The end of the escaped path. */
	escaped[used] = '\0';
}

/* Undoes the escaping of a path; returns 1 when it fits, 0 otherwise. */
static int
trash_unescape(
	const char *escaped,
	char *path,
	size_t size)
{
	size_t used;
	int high;
	int low;

	/* Each character: %XX is a byte, anything else itself. */
	used = 0;
	while (*escaped != '\0' && *escaped != '\r') {
		if (used + 1U >= size)
			return 0;
		high = -1;
		low = -1;
		if (escaped[0] == '%' && escaped[1] != '\0') {
			high = trash_hex(escaped[1]);
			low = trash_hex(escaped[2]);
		}

		/* Two hexadecimal digits after % are a byte; anything else is itself. */
		if (high >= 0 && low >= 0) {
			path[used] = (char)(high * 16 + low);
			escaped += 3;
		} else {
			path[used] = *escaped;
			escaped++;
		}

		/* One more byte of the path. */
		used++;
	}

	/* The end of the path. */
	path[used] = '\0';
	return 1;
}

/* Makes a trash's own folder and its files and info folders; returns 0 or an errno value. */
static int
trash_make_folders(
	const char *trash)
{
	char folder[FM_OPS_PATH_MAX + 8];
	int error;

	/* The trash itself. */
	error = trash_mkdir(trash);
	if (error != 0)
		return error;

	/* The folder of the trashed items. */
	snprintf(folder, sizeof(folder), "%s/files", trash);
	error = trash_mkdir(folder);
	if (error != 0)
		return error;

	/* The folder of their records. */
	snprintf(folder, sizeof(folder), "%s/info", trash);
	error = trash_mkdir(folder);
	if (error != 0)
		return error;

	/* Succeeded: the trash's folders are there. */
	return 0;
}

/* Writes the folder a path is in ("/" for an item of the root, "." for a bare name). */
static void
trash_parent(
	const char *path,
	char *parent,
	size_t size)
{
	char *slash;

	/* The path, cut at its last slash. */
	snprintf(parent, size, "%s", path);
	slash = strrchr(parent, '/');
	if (slash == NULL) {
		/* A bare name is in the current folder. */
		snprintf(parent, size, ".");
	} else if (slash == parent) {
		/* An item of the root is in the root. */
		parent[1] = '\0';
	} else {
		/* Anything else is in the folder before its last slash. */
		*slash = '\0';
	}
}

/* Reports the last part of a path. */
static const char *
trash_base(
	const char *path)
{
	const char *slash;

	/* The part after the last slash, or the whole path. */
	slash = strrchr(path, '/');
	if (slash == NULL)
		return path;

	/* The name after the slash. */
	return slash + 1;
}

/*
 * Finds the top of the volume a trash at the top of a volume belongs to,
 * by the trash's name: $topdir/.Trash-$uid, or $topdir/.Trash/$uid.
 * Returns 0, or ENOENT for any other trash (the home trash).
 */
static int
trash_topdir(
	const char *trash,
	char *topdir,
	size_t size)
{
	char own[64];
	char shared[FM_OPS_PATH_MAX + 16];
	const char *base;
	int match;

	/* The trash's own name, which says which kind it is. */
	base = trash_base(trash);

	/* $topdir/.Trash-$uid is right under the top. */
	snprintf(own, sizeof(own), ".Trash-%lu", (unsigned long)getuid());
	match = strcmp(base, own);
	if (match == 0) {
		trash_parent(trash, topdir, size);
		return 0;
	}

	/* Any other trash but the user's folder of a shared .Trash is no volume's. */
	snprintf(own, sizeof(own), "%lu", (unsigned long)getuid());
	match = strcmp(base, own);
	if (match != 0)
		return ENOENT;

	/* The user's folder must be in a folder called .Trash. */
	trash_parent(trash, shared, sizeof(shared));
	match = strcmp(trash_base(shared), ".Trash");
	if (match != 0)
		return ENOENT;

	/* Succeeded: the top is the folder .Trash is in. */
	trash_parent(shared, topdir, size);
	return 0;
}

/*
 * Finds the top of the volume an item is on: the highest folder above it
 * on the same file system as the folder it is in.  Returns 0, or an errno
 * value.
 */
static int
trash_mount_top(
	const char *item,
	char *topdir,
	size_t size)
{
	struct stat status;
	char current[FM_OPS_PATH_MAX];
	char above[FM_OPS_PATH_MAX];
	dev_t device;
	int error;
	int root;

	/* The folder the item is in, and the file system it is on (the rename happens there). */
	trash_parent(item, current, sizeof(current));
	error = stat(current, &status);
	if (error != 0)
		return errno;
	device = status.st_dev;

	/* Up the folders until the one above is on another file system, or the root. */
	for (;;) {
		root = strcmp(current, "/");
		if (root == 0)
			break;

		/* The folder above, and the file system it is on. */
		trash_parent(current, above, sizeof(above));
		error = stat(above, &status);
		if (error != 0)
			return errno;

		/* The folder above is on another file system: this one is the top. */
		if (status.st_dev != device)
			break;

		/* The same file system goes on up. */
		snprintf(current, sizeof(current), "%s", above);
	}

	/* Succeeded: the top of the item's volume. */
	snprintf(topdir, size, "%s", current);
	return 0;
}

/*
 * Chooses (and makes when missing) the trash at the top of a volume: the
 * user's folder in a shared .Trash the administrator made sticky, else
 * the user's own .Trash-$uid.  Returns 0, or an errno value when neither
 * can be used.
 */
static int
trash_volume(
	const char *topdir,
	char *trash,
	size_t size)
{
	char shared[FM_OPS_PATH_MAX + 16];
	char path[FM_OPS_PATH_MAX + 48];
	unsigned long uid;
	int usable;
	int error;
	int root;
	int written;

	/* The root's top has no slash added to it. */
	uid = (unsigned long)getuid();
	root = strcmp(topdir, "/");
	if (root == 0)
		snprintf(shared, sizeof(shared), "/.Trash");
	else
		snprintf(shared, sizeof(shared), "%s/.Trash", topdir);

	/* The user's folder in a shared, sticky .Trash (not a link). */
	usable = trash_usable(shared, 1);
	if (usable != 0) {
		/* The user's folder there, made when missing, and the user's. */
		snprintf(path, sizeof(path), "%s/%lu", shared, uid);
		error = trash_make_folders(path);
		usable = 0;
		if (error == 0)
			usable = trash_usable(path, 0);

		/* That folder is the trash when it may be used. */
		if (usable != 0) {
			written = snprintf(trash, size, "%s", path);
			if (written < 0 || (size_t)written >= size)
				return ENAMETOOLONG;
			return 0;
		}
	}

	/* The user's own .Trash-$uid, made when missing. */
	if (root == 0)
		snprintf(path, sizeof(path), "/.Trash-%lu", uid);
	else
		snprintf(path, sizeof(path), "%s/.Trash-%lu", topdir, uid);
	error = trash_make_folders(path);
	if (error != 0)
		return error;

	/* One that is a link, or someone else's, is not used. */
	usable = trash_usable(path, 0);
	if (usable == 0)
		return EPERM;

	/* The trash's path for the caller. */
	written = snprintf(trash, size, "%s", path);
	if (written < 0 || (size_t)written >= size)
		return ENAMETOOLONG;

	/* Succeeded: the volume's trash is there. */
	return 0;
}

/*
 * Tells whether a trash folder may be used: a folder, not a link; a
 * shared .Trash must be sticky (so nobody removes another's items), and a
 * user's own folder must be the user's.
 */
static int
trash_usable(
	const char *path,
	int shared)
{
	struct stat status;
	uid_t user;
	int folder;
	int error;

	/* What is there, without following a link. */
	error = lstat(path, &status);
	if (error != 0)
		return 0;

	/* Only a folder (a link is not one under lstat). */
	folder = S_ISDIR(status.st_mode);
	if (folder == 0)
		return 0;

	/* A shared .Trash must be sticky. */
	if (shared != 0) {
		if ((status.st_mode & S_ISVTX) == 0)
			return 0;
		return 1;
	}

	/* A user's own folder must be the user's. */
	user = getuid();
	if (status.st_uid != user)
		return 0;

	/* Succeeded: the folder may be used. */
	return 1;
}

/* Adds a trash to a list unless it is there already; returns 0 or ENOMEM. */
static int
trash_list_add(
	char ***trashes,
	size_t *count,
	const char *path)
{
	char **grown;
	char *copy;
	size_t index;
	int match;

	/* A trash listed already (a volume mounted twice) is not listed again. */
	for (index = 0; index < *count; index++) {
		match = strcmp((*trashes)[index], path);
		if (match == 0)
			return 0;
	}

	/* The path, kept. */
	copy = strdup(path);
	if (copy == NULL)
		return ENOMEM;

	/* Room for one more. */
	grown = realloc(*trashes, (*count + 1U) * sizeof(grown[0]));
	if (grown == NULL) {
		free(copy);
		return ENOMEM;
	}

	/* Succeeded: the trash is listed. */
	grown[*count] = copy;
	*trashes = grown;
	(*count)++;
	return 0;
}

/*
 * Frees a list of trashes (as fm_paths_free does; this file keeps its own
 * so that Image Viewer can build it without the clipboard, ws128-p005).
 */
static void
trash_list_free(
	char **trashes,
	size_t count)
{
	size_t index;

	/* Each path, then the table. */
	for (index = 0; index < count; index++)
		free(trashes[index]);
	free(trashes);
}

/* Adds the trashes at the top of one volume that are there; returns 0 or ENOMEM. */
static int
trash_list_volume(
	char ***trashes,
	size_t *count,
	const char *topdir)
{
	char shared[FM_OPS_PATH_MAX + 16];
	char path[FM_OPS_PATH_MAX + 48];
	unsigned long uid;
	int usable;
	int error;
	int root;

	/* The root's top has no slash added to it. */
	uid = (unsigned long)getuid();
	root = strcmp(topdir, "/");
	if (root == 0)
		snprintf(shared, sizeof(shared), "/.Trash");
	else
		snprintf(shared, sizeof(shared), "%s/.Trash", topdir);

	/* The user's folder in a shared, sticky .Trash, when both are there. */
	usable = trash_usable(shared, 1);
	if (usable != 0) {
		snprintf(path, sizeof(path), "%s/%lu", shared, uid);
		error = trash_list_volume_add(trashes, count, path);
		if (error != 0)
			return error;
	}

	/* The user's own .Trash-$uid. */
	if (root == 0)
		snprintf(path, sizeof(path), "/.Trash-%lu", uid);
	else
		snprintf(path, sizeof(path), "%s/.Trash-%lu", topdir, uid);
	error = trash_list_volume_add(trashes, count, path);
	if (error != 0)
		return error;

	/* Succeeded: the volume's trashes are listed. */
	return 0;
}

/* Adds a user's trash folder of a volume to a list when it may be used; returns 0 or ENOMEM. */
static int
trash_list_volume_add(
	char ***trashes,
	size_t *count,
	const char *path)
{
	int usable;
	int error;

	/* A folder that is missing, a link or someone else's is left out. */
	usable = trash_usable(path, 0);
	if (usable == 0)
		return 0;

	/* The trash is listed. */
	error = trash_list_add(trashes, count, path);
	if (error != 0)
		return error;

	/* Succeeded: the trash is listed. */
	return 0;
}

/*
 * Chooses the trash of the volume an item is on, when that is another
 * file system than the home trash's.  Returns 0 with the trash, or an
 * errno value when the home trash is to be used.
 */
static int
trash_elsewhere(
	const char *item,
	const char *home,
	char *trash,
	size_t size)
{
	struct stat item_status;
	struct stat home_status;
	char files[FM_OPS_PATH_MAX + 8];
	char topdir[FM_OPS_PATH_MAX];
	int system;
	int error;

	/* The file system the item is on. */
	error = lstat(item, &item_status);
	if (error != 0)
		return errno;

	/* The file system of the home trash's items. */
	snprintf(files, sizeof(files), "%s/files", home);
	error = stat(files, &home_status);
	if (error != 0)
		return errno;

	/* An item on the home trash's file system is renamed into it. */
	if (item_status.st_dev == home_status.st_dev)
		return EXDEV;

	/* The top of the item's volume. */
	error = trash_mount_top(item, topdir, sizeof(topdir));
	if (error != 0)
		return error;

	/* A system's mount gets no trash of its own. */
	system = trash_system_top(topdir);
	if (system != 0)
		return EXDEV;

	/* The trash there, made when missing. */
	error = trash_volume(topdir, trash, size);
	if (error != 0)
		return error;

	/* Succeeded: the item goes into its volume's trash. */
	return 0;
}

/* Tells whether the top of a volume is one of the system's mounts, which get no trash of their own. */
static int
trash_system_top(
	const char *topdir)
{
	size_t index;
	size_t length;
	int match;

	/* The tops named whole. */
	for (index = 0; index < sizeof(trash_system_tops) / sizeof(trash_system_tops[0]); index++) {
		match = strcmp(topdir, trash_system_tops[index]);
		if (match == 0)
			return 1;
	}

	/* The folders and what is under them. */
	for (index = 0; index < sizeof(trash_system_trees) / sizeof(trash_system_trees[0]); index++) {
		length = strlen(trash_system_trees[index]);
		match = strncmp(topdir, trash_system_trees[index], length);
		if (match != 0)
			continue;

		/* The folder itself, or a path below it (not a longer name beside it). */
		if (topdir[length] == '\0' || topdir[length] == '/')
			return 1;
	}

	/* A volume of the user's. */
	return 0;
}
