/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The folder of Image Viewer (ws091): the image files of the folder the
 * shown image is in, in the order they are gone through (by name, the runs
 * of digits compared as numbers, "IMG_2" before "IMG_10", letters without
 * regard to case), and which of them is shown.  Hidden files are left out.
 */

#include "imageview.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* How many names the list grows by when it is full. */
#define FOLDER_GROWTH		64U

static int folder_sort(const void *left, const void *right);
static int folder_lower(int character);
static int folder_digit(int character);
static const char *folder_skip_zeros(const char *digits);
static size_t folder_run(const char *digits);

/*
 * Reads the image files of the folder a path is in (or of the folder a
 * path names), and finds the path among them.
 *
 * Returns 0 with the list read and folder->index the path's place (0 when
 * the path is the folder, or not an image of it), or an errno value with
 * the folder left empty.
 */
int
iv_folder_read(
	struct iv_folder *folder,
	const char *path)
{
	char (*names)[IV_NAME_MAX];
	char directory[IV_PATH_MAX];
	const char *name;
	struct dirent *entry;
	struct stat status;
	size_t capacity;
	size_t index;
	char *slash;
	DIR *stream;
	int is_directory;
	int error;
	int match;

	/* The old list goes. */
	iv_folder_release(folder);

	/* Whether the path names a folder (a path that cannot be read is taken for a file). */
	is_directory = 0;
	error = stat(path, &status);
	if (error == 0)
		is_directory = S_ISDIR(status.st_mode);

	/* The folder: the path itself when it is one, otherwise the folder it is in, and the file's name. */
	name = "";
	if (is_directory) {
		snprintf(directory, sizeof(directory), "%s", path);
	} else {
		/* The part before the last slash is the folder; a name without one is in the current folder. */
		snprintf(directory, sizeof(directory), "%s", path);
		slash = strrchr(directory, '/');
		if (slash == NULL) {
			snprintf(directory, sizeof(directory), ".");
			name = path;
		} else if (slash == directory) {
			directory[1] = '\0';
			name = path + 1;
		} else {
			*slash = '\0';
			name = strrchr(path, '/') + 1;
		}
	}

	/* The folder's time, to see later that it changed. */
	error = stat(directory, &status);
	if (error != 0)
		return errno;

	/* Its entries. */
	stream = opendir(directory);
	if (stream == NULL)
		return errno;

	/* Each image file that is not hidden joins the list, which grows as it fills. */
	names = NULL;
	capacity = 0;
	folder->count = 0;
	for (;;) {
		entry = readdir(stream);
		if (entry == NULL)
			break;

		/* Hidden files and files of other kinds are left out. */
		if (entry->d_name[0] == '.')
			continue;

		/* Only the names of the kinds shown join. */
		match = iv_image_is_name(entry->d_name);
		if (!match)
			continue;

		/* Room for one more name. */
		if (folder->count == capacity) {
			capacity += FOLDER_GROWTH;
			names = realloc(folder->names, capacity * sizeof(names[0]));
			if (names == NULL) {
				closedir(stream);
				iv_folder_release(folder);
				return ENOMEM;
			}

			/* The grown list. */
			folder->names = names;
		}

		/* The name. */
		snprintf(folder->names[folder->count], IV_NAME_MAX, "%s", entry->d_name);
		folder->count++;
	}

	/* The folder is read. */
	closedir(stream);

	/* The names in the viewer's order. */
	if (folder->count > 1U)
		qsort(folder->names, folder->count, sizeof(folder->names[0]), folder_sort);

	/* The folder, its time, and where the path is in it. */
	snprintf(folder->directory, sizeof(folder->directory), "%s", directory);
	folder->modified = status.st_mtime;
	folder->index = 0;
	for (index = 0; index < folder->count; index++) {
		/* The path's own name is its place. */
		match = strcmp(folder->names[index], name);
		if (match == 0) {
			folder->index = index;
			break;
		}
	}

	/* Succeeded: the folder's images are known. */
	return 0;
}

/*
 * Forgets the folder's list.
 */
void
iv_folder_release(
	struct iv_folder *folder)
{
	/* The names, and every other field. */
	free(folder->names);
	memset(folder, 0, sizeof(*folder));
}

/*
 * Writes the path of one image of the folder.
 *
 * Returns 0, EINVAL for an index past the list, or ENAMETOOLONG.
 */
int
iv_folder_path(
	const struct iv_folder *folder,
	size_t index,
	char *path,
	size_t size)
{
	int length;
	int root;

	/* Only an image of the list. */
	if (index >= folder->count)
		return EINVAL;

	/* The folder and the name, without doubling the root's slash. */
	root = strcmp(folder->directory, "/");
	if (root == 0) {
		length = snprintf(path, size, "/%s", folder->names[index]);
	} else {
		length = snprintf(path, size, "%s/%s", folder->directory, folder->names[index]);
	}

	/* A path that did not fit is refused rather than cut. */
	if (length < 0 || (size_t)length >= size)
		return ENAMETOOLONG;

	/* Succeeded: the path. */
	return 0;
}

/*
 * Compares two names in the viewer's order: runs of digits as numbers
 * (leading zeros aside), other characters without regard to case; names
 * equal so are ordered by their bytes.  Returns less than, equal to or
 * greater than zero.
 */
int
iv_folder_compare(
	const char *left,
	const char *right)
{
	const char *left_start;
	const char *right_start;
	size_t left_length;
	size_t right_length;
	int left_character;
	int right_character;
	int left_digit;
	int right_digit;
	int difference;

	/* Walks both names together. */
	left_start = left;
	right_start = right;
	while (*left != '\0' && *right != '\0') {
		left_character = (unsigned char)*left;
		right_character = (unsigned char)*right;
		left_digit = folder_digit(left_character);
		right_digit = folder_digit(right_character);

		/* Other characters compare without regard to case. */
		if (!left_digit || !right_digit) {
			difference = folder_lower(left_character) - folder_lower(right_character);
			if (difference != 0)
				return difference;

			/* The characters are alike; the next pair decides. */
			left++;
			right++;
			continue;
		}

		/* Two runs of digits compare as numbers: leading zeros count for nothing. */
		left = folder_skip_zeros(left);
		right = folder_skip_zeros(right);

		/* The longer run is the larger number. */
		left_length = folder_run(left);
		right_length = folder_run(right);
		if (left_length < right_length)
			return -1;
		if (left_length > right_length)
			return 1;

		/* Runs of one length compare digit by digit. */
		difference = strncmp(left, right, left_length);
		if (difference != 0)
			return difference;

		/* The numbers are equal; the characters after them decide. */
		left += left_length;
		right += right_length;
	}

	/* A name that ended first comes first. */
	if (*left == '\0' && *right != '\0')
		return -1;
	if (*left != '\0' && *right == '\0')
		return 1;

	/* Equal so far: the bytes decide. */
	difference = strcmp(left_start, right_start);

	/* Reports the order of the bytes. */
	return difference;
}

/* Orders two names of the list for qsort. */
static int
folder_sort(
	const void *left,
	const void *right)
{
	int order;

	/* Compares the two names of the list, each a row of the names table. */
	order = iv_folder_compare((const char *)left, (const char *)right);

	/* Reports which name comes first. */
	return order;
}

/* Lowers an ASCII letter; other characters stay. */
static int
folder_lower(
	int character)
{
	/* An upper-case letter. */
	if (character >= 'A' && character <= 'Z')
		return character - 'A' + 'a';

	/* Anything else as it is. */
	return character;
}

/* Tells whether a character is an ASCII digit. */
static int
folder_digit(
	int character)
{
	/* 0 to 9. */
	if (character >= '0' && character <= '9')
		return 1;

	/* Anything else. */
	return 0;
}

/* Passes the leading zeros of a run of digits, keeping its last digit. */
static const char *
folder_skip_zeros(
	const char *digits)
{
	int next;

	/* Each zero with another digit after it. */
	while (*digits == '0') {
		next = folder_digit((unsigned char)digits[1]);
		if (!next)
			break;

		/* The zero counts for nothing. */
		digits++;
	}

	/* Reports where the number's significant digits start. */
	return digits;
}

/* Measures a run of digits. */
static size_t
folder_run(
	const char *digits)
{
	size_t length;
	int digit;

	/* Counts the digits in a row. */
	length = 0;
	for (;;) {
		digit = folder_digit((unsigned char)digits[length]);
		if (!digit)
			break;

		/* One more digit of the run. */
		length++;
	}

	/* Reports the run's length. */
	return length;
}
