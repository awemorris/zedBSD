/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The log files bootlog keeps, and the reading of the kernel log for them.
 *
 * Each boot gets one file, boot-NNNNNN.txt, numbered one past the newest
 * file an earlier boot left, and only the newest few are kept.  A file is
 * only ever appended to and made durable after each append, so a power
 * cut takes at most the stretch being written, never what is already
 * there.
 */

#include "userland/base/bootlog/log-file.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* What every log file name begins with. */
#define BOOTLOG_NAME_PREFIX "boot-"

/* What every log file name ends with. */
#define BOOTLOG_NAME_SUFFIX ".txt"

/* How many digits the boot number of a log file name has. */
#define BOOTLOG_NAME_DIGITS 6U

/* How many times a new file is tried under the next number when one exists. */
#define BOOTLOG_CREATE_TRIES 8

/*
 * The words of the kernel log that make bootlog keep the log at once.
 *
 * They are the marks of the failures BUG-288 is after: a volume that
 * stopped writing, the SD controller's errors, a UFS message, and a
 * kernel stop.  The longest is BOOTLOG_TRIGGER_OVERLAP plus one bytes.
 */
static const char *const bootlog_trigger_words[] = {
	"read-only",
	"sdhci",
	"ufs:",
	"panic",
};

static int bootlog_numbers_list(const char *directory, unsigned long **numbers, size_t *count, unsigned long *highest);
static int bootlog_number_compare(const void *left, const void *right);
static int bootlog_path_join(const char *directory, const char *name, char *path, size_t size);
static void bootlog_numbers_prune(const char *directory, unsigned long *numbers, size_t count, unsigned keep);
static int bootlog_directory_sync(const char *directory);
static int bootlog_write_whole(int descriptor, const char *bytes, size_t length);

/*
 * Reads the boot number from a log file name.
 *
 * The name is boot-NNNNNN.txt with exactly six digits; the letters may be
 * of either case, as a FAT volume written elsewhere may have them.
 */
int
bootlog_name_number(
	const char *name,
	unsigned long *number)
{
	size_t prefix_length;
	size_t suffix_length;
	size_t length;
	unsigned long value;
	unsigned index;
	int prefix_differs;
	int suffix_differs;
	char digit;

	prefix_length = strlen(BOOTLOG_NAME_PREFIX);
	suffix_length = strlen(BOOTLOG_NAME_SUFFIX);

	/* Refuses a name of any other length. */
	length = strlen(name);
	if (length != prefix_length + BOOTLOG_NAME_DIGITS + suffix_length)
		return -1;

	/* Refuses a name that does not begin as a log file name does. */
	prefix_differs = strncasecmp(name, BOOTLOG_NAME_PREFIX, prefix_length);
	if (prefix_differs != 0)
		return -1;

	/* Refuses a name that does not end as a log file name does. */
	suffix_differs = strncasecmp(name + prefix_length + BOOTLOG_NAME_DIGITS,
				     BOOTLOG_NAME_SUFFIX,
				     suffix_length);
	if (suffix_differs != 0)
		return -1;

	/* Reads the digits of the boot number. */
	value = 0;
	for (index = 0; index < BOOTLOG_NAME_DIGITS; index++) {
		digit = name[prefix_length + index];

		/* Refuses a name with anything but a digit there. */
		if (digit < '0' || digit > '9')
			return -1;

		value = value * 10UL + (unsigned long)(digit - '0');
	}

	/* Succeeded: the name is a log file's, of this boot number. */
	*number = value;
	return 0;
}

/*
 * Writes the log file name of a boot number.
 */
int
bootlog_name_format(
	unsigned long number,
	char *name,
	size_t size)
{
	int written;

	/* Refuses a number the six digits cannot hold. */
	if (number > BOOTLOG_NUMBER_MAX)
		return -1;

	/* Writes the name and refuses a buffer it does not fit. */
	written = snprintf(name,
			   size,
			   "%s%06lu%s",
			   BOOTLOG_NAME_PREFIX,
			   number,
			   BOOTLOG_NAME_SUFFIX);
	if (written < 0 || (size_t)written >= size)
		return -1;

	/* Succeeded: the name is in the buffer. */
	return 0;
}

/*
 * Creates this boot's log file in the directory and opens it for appending.
 *
 * The directory is made if it is missing.  The new file is numbered one
 * past the newest an earlier boot left, and the oldest files are then
 * removed until only the newest keep files, the new one among them, are
 * left.  Reports the open descriptor and the boot number, or -1 with
 * errno set.
 */
int
bootlog_file_create(
	const char *directory,
	unsigned keep,
	unsigned long *number)
{
	char path[PATH_MAX];
	char name[BOOTLOG_NAME_SIZE];
	unsigned long *numbers;
	unsigned long highest;
	unsigned long next;
	size_t count;
	int descriptor;
	int made;
	int listed;
	int formatted;
	int joined;
	int tries;
	int saved_errno;

	/* Makes the directory the logs live in, on the first boot that keeps one. */
	made = mkdir(directory, 0755);
	if (made != 0 && errno != EEXIST)
		return -1;

	/* Lists the logs earlier boots left. */
	listed = bootlog_numbers_list(directory, &numbers, &count, &highest);
	if (listed != 0)
		return -1;

	/* Creates the log of this boot under the first free number past the newest. */
	next = highest + 1UL;
	descriptor = -1;
	for (tries = 0; tries < BOOTLOG_CREATE_TRIES; tries++) {
		/* Names the file, and gives up past the last number a name holds. */
		formatted = bootlog_name_format(next, name, sizeof(name));
		if (formatted != 0) {
			free(numbers);
			errno = ENOSPC;
			return -1;
		}

		/* Puts the name in the directory. */
		joined = bootlog_path_join(directory, name, path, sizeof(path));
		if (joined != 0) {
			free(numbers);
			errno = ENAMETOOLONG;
			return -1;
		}

		/* A file of that number made since the listing moves the number on. */
		descriptor = open(path,
				  O_WRONLY | O_CREAT | O_EXCL | O_APPEND | O_CLOEXEC,
				  0644);
		if (descriptor >= 0)
			break;

		/* Gives up on any failure but a file already there. */
		if (errno != EEXIST) {
			saved_errno = errno;
			free(numbers);
			errno = saved_errno;
			return -1;
		}

		next++;
	}

	/* Gives up when every number tried was taken. */
	if (descriptor < 0) {
		free(numbers);
		errno = EEXIST;
		return -1;
	}

	/* Removes the oldest logs beyond the number kept. */
	bootlog_numbers_prune(directory, numbers, count, keep);
	free(numbers);

	/*
	 * Makes the new entry and the removals durable now, so the file is
	 * there even when the machine loses power before its first append.
	 */
	(void)bootlog_directory_sync(directory);

	/* Succeeded: the caller appends to the new file. */
	*number = next;
	return descriptor;
}

/*
 * Appends a header and a stretch of the kernel log, and makes them durable.
 */
int
bootlog_file_append(
	int descriptor,
	const char *header,
	const char *text,
	size_t length)
{
	int written;
	int synced;

	/* Writes the header that says why and when this stretch is kept. */
	written = bootlog_write_whole(descriptor, header, strlen(header));
	if (written != 0)
		return -1;

	/* Writes the stretch of the kernel log. */
	written = bootlog_write_whole(descriptor, text, length);
	if (written != 0)
		return -1;

	/* Puts both on the volume before the machine can lose them. */
	synced = fsync(descriptor);
	if (synced != 0)
		return -1;

	/* Succeeded: the stretch is on the volume. */
	return 0;
}

/*
 * Finds where the text not yet kept begins in a copy of the kernel log.
 *
 * The kernel log is a ring: dropped counts the bytes it has let go of
 * since the boot, so the copy holds the bytes from dropped to dropped
 * plus length of everything ever logged.  seen_total is how far the
 * earlier copies were kept.  Reports the offset in the copy and, through
 * lost, how many bytes went before they could be kept.
 */
size_t
bootlog_unseen_offset(
	uint64_t seen_total,
	uint64_t dropped,
	size_t length,
	uint64_t *lost)
{
	uint64_t total;

	total = dropped + (uint64_t)length;
	*lost = 0;

	/* Nothing is new when everything in the copy was kept before. */
	if (seen_total >= total)
		return length;

	/* The ring let go of what came after the last kept byte. */
	if (seen_total < dropped) {
		*lost = dropped - seen_total;
		return 0;
	}

	/* Succeeded: the new text begins where the kept text ends. */
	return (size_t)(seen_total - dropped);
}

/*
 * Finds the first trigger word in a stretch of the kernel log.
 *
 * Case is ignored.  Reports the word found, or NULL when there is none.
 */
const char *
bootlog_trigger_find(
	const char *text,
	size_t length)
{
	const char *word;
	size_t word_length;
	size_t words;
	size_t index;
	size_t at;
	int differs;

	/* Looks for each word over the whole stretch. */
	words = sizeof(bootlog_trigger_words) / sizeof(bootlog_trigger_words[0]);
	for (index = 0; index < words; index++) {
		word = bootlog_trigger_words[index];
		word_length = strlen(word);

		/* A word longer than the stretch is not in it. */
		if (word_length > length)
			continue;

		/* Tries the word at every place it fits. */
		for (at = 0; at + word_length <= length; at++) {
			/* Reports the word where it matches, case aside. */
			differs = strncasecmp(text + at, word, word_length);
			if (differs == 0)
				return word;
		}
	}

	/* No trigger word is in the stretch. */
	return NULL;
}

/* Lists the boot numbers of the log files in the directory. */
static int
bootlog_numbers_list(
	const char *directory,
	unsigned long **numbers,
	size_t *count,
	unsigned long *highest)
{
	DIR *stream;
	struct dirent *entry;
	unsigned long *list;
	unsigned long *grown;
	unsigned long number;
	size_t used;
	size_t room;
	int parsed;
	int saved_errno;

	list = NULL;
	used = 0;
	room = 0;
	*highest = 0;

	/* Opens the directory to read its names. */
	stream = opendir(directory);
	if (stream == NULL)
		return -1;

	/* Collects the number of every name that is a log file's. */
	errno = 0;
	entry = readdir(stream);
	while (entry != NULL) {
		/* Keeps the number of a name that is a log file's. */
		parsed = bootlog_name_number(entry->d_name, &number);
		if (parsed == 0) {
			/* Grows the list by doubling when it is full. */
			if (used == room) {
				room = room == 0 ? 16U : room * 2U;
				grown = realloc(list, room * sizeof(*list));
				if (grown == NULL) {
					free(list);
					closedir(stream);
					errno = ENOMEM;
					return -1;
				}
				list = grown;
			}

			list[used] = number;
			used++;

			/* Remembers the newest boot. */
			if (number > *highest)
				*highest = number;
		}

		/* Reads the next name; errno tells the end from a failure. */
		errno = 0;
		entry = readdir(stream);
	}

	/* Refuses a listing that ended on a read error rather than the end. */
	if (errno != 0) {
		saved_errno = errno;
		free(list);
		closedir(stream);
		errno = saved_errno;
		return -1;
	}

	closedir(stream);

	/* Succeeded: the caller frees the list. */
	*numbers = list;
	*count = used;
	return 0;
}

/* Orders two boot numbers from the oldest to the newest. */
static int
bootlog_number_compare(
	const void *left,
	const void *right)
{
	unsigned long left_number;
	unsigned long right_number;

	left_number = *(const unsigned long *)left;
	right_number = *(const unsigned long *)right;

	/* The smaller number is the older boot. */
	if (left_number < right_number)
		return -1;
	if (left_number > right_number)
		return 1;

	/* The same boot. */
	return 0;
}

/* Joins a directory and a name into a path that fits the buffer. */
static int
bootlog_path_join(
	const char *directory,
	const char *name,
	char *path,
	size_t size)
{
	int written;

	/* Writes the path and refuses a buffer it does not fit. */
	written = snprintf(path, size, "%s/%s", directory, name);
	if (written < 0 || (size_t)written >= size)
		return -1;

	/* Succeeded: the path is in the buffer. */
	return 0;
}

/*
 * Removes the oldest log files until keep are left with the new one.
 *
 * A file that cannot be removed is reported and passed over; the new log
 * is kept either way.
 */
static void
bootlog_numbers_prune(
	const char *directory,
	unsigned long *numbers,
	size_t count,
	unsigned keep)
{
	char path[PATH_MAX];
	char name[BOOTLOG_NAME_SIZE];
	size_t remaining;
	size_t index;
	int formatted;
	int joined;
	int removed;

	/* Puts the oldest boot first. */
	if (count > 1U)
		qsort(numbers, count, sizeof(*numbers), bootlog_number_compare);

	/* Removes from the oldest while more than keep files would be left. */
	remaining = count + 1U;
	for (index = 0; index < count && remaining > keep; index++) {
		/* Names the file of that boot. */
		formatted = bootlog_name_format(numbers[index], name, sizeof(name));
		if (formatted != 0)
			continue;
		joined = bootlog_path_join(directory, name, path, sizeof(path));
		if (joined != 0)
			continue;

		/* Counts the file as gone only when it went. */
		removed = unlink(path);
		if (removed != 0) {
			fprintf(stderr, "bootlog: cannot remove %s: %s\n",
				path,
				strerror(errno));
			continue;
		}

		remaining--;
	}
}

/* Makes a directory's entries durable on its volume. */
static int
bootlog_directory_sync(
	const char *directory)
{
	int descriptor;
	int synced;
	int saved_errno;

	/* Opens the directory itself. */
	descriptor = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (descriptor < 0)
		return -1;

	/* Writes its entries out. */
	synced = fsync(descriptor);
	saved_errno = errno;
	close(descriptor);
	if (synced != 0) {
		errno = saved_errno;
		return -1;
	}

	/* Succeeded: the entries are on the volume. */
	return 0;
}

/* Writes every byte of a buffer, through short writes and interruptions. */
static int
bootlog_write_whole(
	int descriptor,
	const char *bytes,
	size_t length)
{
	ssize_t written;
	size_t done;

	/* Writes until the whole buffer is out. */
	done = 0;
	while (done < length) {
		written = write(descriptor, bytes + done, length - done);
		if (written < 0) {
			/* A signal cut the write short before anything was written. */
			if (errno == EINTR)
				continue;

			return -1;
		}

		/* A write that made no progress would loop for ever. */
		if (written == 0) {
			errno = EIO;
			return -1;
		}

		done += (size_t)written;
	}

	/* Succeeded: every byte is written. */
	return 0;
}
