/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Lists directory contents (POSIX XCU ls).
 *
 *	ls [-adFhiLlRrt1C] [file...]
 *
 * With no operand the current directory is listed.  Operands that are not
 * directories (all of them with -d) are listed first as one sorted group,
 * then each directory's contents, with its name as a header when there
 * are several operands.  -l writes the long format, -C columns 80 wide,
 * and -1 (the default) one name to a line.
 */

#include "userland/base/common/command.h"
#include <dirent.h>
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* The longest path ls builds by joining a directory and a name. */
#define LS_PATH_CAPACITY 1024U

/* How deep -R goes before it reports a loop. */
#define LS_RECURSION_LIMIT 64

/* The width of the terminal that -C fills. */
#define LS_COLUMNS_WIDTH 80U

/* The age past which the long format shows the year instead of the time: six months. */
#define LS_RECENT_SECONDS 15552000

/*
 * The options of one run of ls, each 0 or 1.  main() fills it from the
 * command line, and everything else only reads it.
 */
struct options {
	int all;		/* -a: names starting with a dot too */
	int directory;		/* -d: a directory operand as itself */
	int classify;		/* -F: a mark after the name for the type */
	int human;		/* -h: sizes with a unit */
	int long_format;	/* -l: the long format */
	int recursive;		/* -R: the subdirectories too */
	int reverse;		/* -r: the order reversed */
	int time_sort;		/* -t: newest first */
	int one;		/* -1 (and -l): one name to a line */
	int columns;		/* -C: names in columns */
	int inode;		/* -i: the file serial number first */
	int follow;		/* -L: what a symbolic link points to */
};

/*
 * One name to list, with its status.  The name is allocated by the entry,
 * except for the one list_operand() builds around its operand.
 */
struct entry {
	char *name;
	struct stat status;
	int status_valid;	/* 0 when the status could not be read */
};

/*
 * The widths of the columns of the long format, the widest value of each
 * among the entries listed together.
 */
struct long_widths {
	size_t links;
	size_t user;
	size_t group;
	size_t size;
};

static int finish_output(int failed);
static int list_operands(int count, char **names, const struct options *options);
static int operand_status(const char *name, const struct options *options, struct stat *status);
static int list_operand(const char *path, const struct options *options, int header);
static int list_directory(const char *path, const struct options *options, int header, int depth);
static int list_subdirectories(const char *path, const struct entry *items, size_t count, const struct options *options, int depth);
static int load(const char *path, const struct options *options, struct entry **result, size_t *result_count);
static int read_entries(DIR *directory, const char *path, const struct options *options, struct entry **items, size_t *count);
static char *copy_string(const char *text);
static int join_path(const char *directory, const char *name, char *out, size_t capacity);
static void sort_entries(struct entry *items, size_t count, const struct options *options);
static int compare(const struct entry *left, const struct entry *right, const struct options *options);
static int print_entries(const char *path, struct entry *items, size_t count, const struct options *options);
static int print_long_entries(const char *path, struct entry *items, size_t count, const struct options *options);
static void print_columns(const struct entry *items, size_t count, const struct options *options);
static void measure_long(const struct entry *items, size_t count, const struct options *options, struct long_widths *widths);
static void human_size(off_t value, char out[16]);
static const char *uid_name(uid_t id, char out[24]);
static const char *gid_name(gid_t id, char out[24]);
static int print_long(const char *directory, const struct entry *item, const struct options *options, const struct long_widths *widths);
static void mode_text(mode_t mode, char out[11]);
static char type_char(mode_t mode);
static void ls_time(time_t value, char out[32]);
static int days_in_year(long long year);
static int days_in_month(int month, long long year);
static void print_name(const struct entry *item, const struct options *options);
static size_t inode_width(const struct entry *item, const struct options *options);
static char suffix(mode_t mode);
static void free_entries(struct entry *items, size_t count);

/*
 * Runs ls.
 */
int
main(
	int argc,
	char **argv)
{
	struct options options;
	const char *letter;
	int index;
	int failed;
	int listed;
	int operands;
	int compare_dashes;

	/* Each option word, which may join several letters; -- ends them. */
	memset(&options, 0, sizeof(options));
	for (index = 1; index < argc; index++) {
		if (argv[index][0] != '-' || argv[index][1] == '\0')
			break;
		compare_dashes = strcmp(argv[index], "--");
		if (compare_dashes == 0) {
			index++;
			break;
		}

		/* Each letter of the word. */
		for (letter = argv[index] + 1; *letter != '\0'; letter++) {
			/* Chooses the option by its letter. */
			switch (*letter) {
			case 'a':
				options.all = 1;
				break;
			case 'd':
				options.directory = 1;
				break;
			case 'F':
				options.classify = 1;
				break;
			case 'h':
				options.human = 1;
				break;
			case 'i':
				options.inode = 1;
				break;
			case 'L':
				options.follow = 1;
				break;
			case 'l':
				options.long_format = 1;
				options.one = 1;
				break;
			case 'R':
				options.recursive = 1;
				break;
			case 'r':
				options.reverse = 1;
				break;
			case 't':
				options.time_sort = 1;
				break;
			case '1':
				options.one = 1;
				options.columns = 0;
				break;
			case 'C':
				options.columns = 1;
				options.one = 0;
				break;
			default:
				fprintf(stderr, "usage: ls [-adFhiLlRrt1C] [file...]\n");
				return 1;
			}
		}
	}

	/* No operand lists the current directory. */
	operands = argc - index;
	if (operands == 0) {
		listed = list_operand(".", &options, 0);
		failed = 1;
		if (listed)
			failed = 0;
		failed = finish_output(failed);
		return failed;
	}

	/* The operands: files first as one sorted group, then each directory. */
	failed = list_operands(operands, argv + index, &options);
	failed = finish_output(failed);

	/* Reports a failure, which the status already says. */
	if (failed != 0)
		return failed;

	/* Succeeded. */
	return 0;
}

/*
 * Writes out what is still buffered, so that a failed write is reported
 * instead of a partial listing; returns the status ls ends with.
 */
static int
finish_output(
	int failed)
{
	int error;
	int stream_error;

	/* A failed write, now or earlier, is status 1. */
	error = fflush(stdout);
	if (error != 0)
		return 1;
	stream_error = ferror(stdout);
	if (stream_error)
		return 1;

	/* Reports the listing's own failure. */
	if (failed != 0)
		return failed;

	/* Succeeded. */
	return 0;
}

/*
 * Lists the operands as POSIX orders them: the missing ones reported first,
 * then every file (or directory with -d) as one sorted group, then each
 * directory in sorted order with its name as a header when there are
 * several operands.  Returns 0, 2 when an operand could not be accessed
 * (as GNU ls does), and 1 for any other failure.
 */
static int
list_operands(
	int count,
	char **names,
	const struct options *options)
{
	struct long_widths widths;
	struct entry *files;
	struct entry *directories;
	struct entry *entry;
	struct stat status;
	size_t file_count;
	size_t directory_count;
	size_t index;
	int error;
	int failed;
	int directory;
	int header;
	int listed;

	/* Room for every operand in either group. */
	files = calloc((size_t)count, sizeof(*files));
	directories = calloc((size_t)count, sizeof(*directories));
	if (files == NULL || directories == NULL) {
		fprintf(stderr, "ls: out of memory\n");
		free(files);
		free(directories);
		return 1;
	}

	/* Each operand into its group; a missing one is reported now. */
	failed = 0;
	file_count = 0;
	directory_count = 0;
	for (index = 0; index < (size_t)count; index++) {
		error = operand_status(names[index], options, &status);
		if (error != 0) {
			command_error("ls", names[index]);
			failed = 2;
			continue;
		}

		/* A directory is listed by its contents, unless -d. */
		directory = S_ISDIR(status.st_mode);
		if (options->directory)
			directory = 0;
		if (directory) {
			entry = &directories[directory_count];
			directory_count++;
		} else {
			entry = &files[file_count];
			file_count++;
		}

		/* The entry, with the status already read. */
		entry->name = copy_string(names[index]);
		entry->status = status;
		entry->status_valid = 1;
	}

	/* The files, sorted together; in the long format without a total. */
	sort_entries(files, file_count, options);
	if (file_count > 0 && options->long_format) {
		measure_long(files, file_count, options, &widths);
		for (index = 0; index < file_count; index++) {
			listed = print_long("", &files[index], options, &widths);
			if (!listed && failed == 0)
				failed = 1;
		}
	} else if (file_count > 0) {
		print_entries("", files, file_count, options);
	}

	/* Each directory, after a blank line when something came before it. */
	sort_entries(directories, directory_count, options);
	header = 0;
	if (count > 1)
		header = 1;
	for (index = 0; index < directory_count; index++) {
		if (file_count > 0 || index > 0)
			putchar('\n');
		listed = list_directory(directories[index].name, options, header, 0);
		if (!listed && failed == 0)
			failed = 1;
	}

	/* The groups are done with. */
	free_entries(files, file_count);
	free_entries(directories, directory_count);

	/* Reports whether any operand failed. */
	if (failed != 0)
		return failed;

	/* Succeeded. */
	return 0;
}

/*
 * Reads the status of a path: of what a symbolic link points to with -L,
 * and of the link itself otherwise; with -L a dangling link is still
 * listed as a link.  Returns 0 or -1.
 */
static int
operand_status(
	const char *name,
	const struct options *options,
	struct stat *status)
{
	int error;

	/* -L follows the link when it leads somewhere. */
	if (options->follow) {
		error = stat(name, status);
		if (error == 0)
			return 0;
	}

	/* The entry itself. */
	error = lstat(name, status);
	if (error != 0)
		return -1;

	/* Succeeded. */
	return 0;
}

/*
 * Lists the one implicit operand (the current directory): its contents,
 * or itself with -d.  Returns 1, or 0 after a message.
 */
static int
list_operand(
	const char *path,
	const struct options *options,
	int header)
{
	struct long_widths widths;
	struct stat status;
	struct entry item;
	int error;
	int directory;
	int listed;

	/* The operand's own status. */
	error = lstat(path, &status);
	if (error != 0) {
		command_error("ls", path);
		return 0;
	}

	/* A directory is listed by its contents, unless -d. */
	directory = S_ISDIR(status.st_mode);
	if (directory && !options->directory) {
		listed = list_directory(path, options, header, 0);
		if (!listed)
			return 0;
		return 1;
	}

	/* Anything else is listed as itself. */
	memset(&item, 0, sizeof(item));
	item.name = (char *)path;
	item.status = status;
	item.status_valid = 1;

	/* In the long format. */
	if (options->long_format) {
		measure_long(&item, 1, options, &widths);
		listed = print_long("", &item, options, &widths);
		if (!listed)
			return 0;
		return 1;
	}

	/* Succeeded: by its name. */
	print_name(&item, options);
	putchar('\n');
	return 1;
}

/*
 * Lists the contents of a directory, with its name as a header when asked,
 * and with -R the subdirectories after it.  Returns 1, or 0 when anything
 * could not be listed.
 */
static int
list_directory(
	const char *path,
	const struct options *options,
	int header,
	int depth)
{
	struct entry *items;
	size_t count;
	int loaded;
	int printed;
	int ok;

	/* -R through a loop of links ends somewhere. */
	if (depth > LS_RECURSION_LIMIT) {
		errno = ELOOP;
		command_error("ls", path);
		return 0;
	}

	/* The entries, sorted. */
	loaded = load(path, options, &items, &count);
	if (!loaded) {
		command_error("ls", path);
		return 0;
	}

	/* The header, then the entries. */
	ok = 1;
	if (header)
		printf("%s:\n", path);
	printed = print_entries(path, items, count, options);
	if (!printed)
		ok = 0;

	/* -R: each subdirectory after this one. */
	if (options->recursive) {
		printed = list_subdirectories(path, items, count, options, depth);
		if (!printed)
			ok = 0;
	}

	/* The entries are done with. */
	free_entries(items, count);

	/* Reports what could not be listed. */
	if (!ok)
		return 0;

	/* Succeeded. */
	return 1;
}

/* Lists each subdirectory among a directory's entries (-R); returns 1, or 0 on a failure. */
static int
list_subdirectories(
	const char *path,
	const struct entry *items,
	size_t count,
	const struct options *options,
	int depth)
{
	char child[LS_PATH_CAPACITY];
	size_t index;
	int directory;
	int dot;
	int dot_dot;
	int joined;
	int listed;
	int ok;

	/* Each entry that is a directory, other than . and .. */
	ok = 1;
	for (index = 0; index < count; index++) {
		if (!items[index].status_valid)
			continue;
		directory = S_ISDIR(items[index].status.st_mode);
		if (!directory)
			continue;
		dot = strcmp(items[index].name, ".");
		dot_dot = strcmp(items[index].name, "..");
		if (dot == 0 || dot_dot == 0)
			continue;

		/* The path of the subdirectory. */
		joined = join_path(path, items[index].name, child, sizeof(child));
		if (!joined) {
			command_error("ls", items[index].name);
			ok = 0;
			continue;
		}

		/* A blank line, then the subdirectory under its own header. */
		putchar('\n');
		listed = list_directory(child, options, 1, depth + 1);
		if (!listed)
			ok = 0;
	}

	/* Reports what could not be listed. */
	if (!ok)
		return 0;

	/* Succeeded. */
	return 1;
}

/*
 * Reads the entries of a directory with their status, sorted.  Returns 1
 * with an array the caller frees with free_entries(), or 0 with errno set
 * and nothing to free.
 */
static int
load(
	const char *path,
	const struct options *options,
	struct entry **result,
	size_t *result_count)
{
	struct entry *items;
	DIR *directory;
	size_t count;
	int read;
	int closed;
	int saved;

	/* The directory. */
	directory = opendir(path);
	if (directory == NULL)
		return 0;

	/* Its entries; a partial list is not published after a read error. */
	items = NULL;
	count = 0;
	read = read_entries(directory, path, options, &items, &count);
	if (!read) {
		saved = errno;
		(void)closedir(directory);
		free_entries(items, count);
		errno = saved;
		return 0;
	}

	/* A failure to close is a failure of the directory too. */
	closed = closedir(directory);
	if (closed != 0) {
		saved = errno;
		free_entries(items, count);
		errno = saved;
		return 0;
	}

	/* Succeeded: the entries, sorted. */
	sort_entries(items, count, options);
	*result = items;
	*result_count = count;
	return 1;
}

/*
 * Reads the entries of an open directory into a growing array, with -a
 * starting with . and .. and otherwise leaving out the names starting with
 * a dot.  Returns 1, or 0 with errno set; either way *items and *count
 * describe what was read.
 */
static int
read_entries(
	DIR *directory,
	const char *path,
	const struct options *options,
	struct entry **items,
	size_t *count)
{
	static const char *const dots[] = {".", ".."};
	char child[LS_PATH_CAPACITY];
	struct dirent *found;
	struct entry *larger;
	const char *name;
	size_t capacity;
	unsigned dot;
	int dot_name;
	int dot_dot_name;
	int joined;
	int error;

	/* Each name until the end of the directory. */
	capacity = 0;
	dot = 0;
	for (;;) {
		if (options->all && dot < 2U) {
			/* -a lists . and .. first, whatever order the directory has them in. */
			name = dots[dot];
			dot++;
		} else {
			/* errno tells a read error from the end of the directory. */
			errno = 0;
			found = readdir(directory);
			if (found == NULL && errno != 0)
				return 0;
			if (found == NULL)
				break;
			name = found->d_name;

			/* Without -a a name starting with a dot is hidden. */
			if (!options->all && name[0] == '.')
				continue;

			/* With -a, . and .. have been listed already. */
			dot_name = strcmp(name, ".");
			dot_dot_name = strcmp(name, "..");
			if (options->all && (dot_name == 0 || dot_dot_name == 0))
				continue;
		}

		/* The array doubles when it is full. */
		if (*count == capacity) {
			capacity = 16U;
			if (*count != 0)
				capacity = *count * 2U;
			larger = realloc(*items, capacity * sizeof(**items));
			if (larger == NULL)
				return 0;
			*items = larger;
		}

		/* The name. */
		(*items)[*count].name = copy_string(name);
		if ((*items)[*count].name == NULL)
			return 0;

		/* Its status, which a path too long for the buffer leaves unread. */
		(*items)[*count].status_valid = 0;
		joined = join_path(path, name, child, sizeof(child));
		if (joined) {
			error = operand_status(child, options, &(*items)[*count].status);
			if (error == 0)
				(*items)[*count].status_valid = 1;
		}

		/* The entry is in the array. */
		(*count)++;
	}

	/* Succeeded: the whole directory. */
	return 1;
}

/* Returns an allocated copy of a string, or NULL without memory. */
static char *
copy_string(
	const char *text)
{
	size_t size;
	char *copy;

	/* Room for the text and its terminating null. */
	size = strlen(text) + 1U;
	copy = malloc(size);
	if (copy == NULL)
		return NULL;

	/* Succeeded: the copy. */
	memcpy(copy, text, size);
	return copy;
}

/*
 * Writes directory/name into a buffer, with a slash between them unless
 * the directory is empty or ends in one.  Returns 1, or 0 with errno
 * ENAMETOOLONG when it does not fit.
 */
static int
join_path(
	const char *directory,
	const char *name,
	char *out,
	size_t capacity)
{
	size_t directory_length;
	size_t name_length;
	size_t slash;

	/* A slash is needed after a directory that does not end in one. */
	directory_length = strlen(directory);
	name_length = strlen(name);
	slash = 0;
	if (directory_length > 0 && directory[directory_length - 1U] != '/')
		slash = 1;

	/* The path must fit with its terminating null. */
	if (directory_length + slash + name_length + 1U > capacity) {
		errno = ENAMETOOLONG;
		return 0;
	}

	/* Succeeded: the directory, the slash and the name. */
	memcpy(out, directory, directory_length);
	if (slash)
		out[directory_length] = '/';
	memcpy(out + directory_length + slash, name, name_length + 1U);
	return 1;
}

/* Sorts entries in the order compare() gives, keeping equal ones in place. */
static void
sort_entries(
	struct entry *items,
	size_t count,
	const struct options *options)
{
	struct entry value;
	size_t index;
	size_t at;
	int order;

	/* An insertion sort: each entry moves back past the ones that follow it in order. */
	for (index = 1; index < count; index++) {
		value = items[index];
		at = index;
		while (at > 0) {
			order = compare(&items[at - 1U], &value, options);
			if (order <= 0)
				break;
			items[at] = items[at - 1U];
			at--;
		}

		/* The entry in its place. */
		items[at] = value;
	}
}

/*
 * Orders two entries: by name, or with -t newest first and then by name;
 * -r reverses the order.  Returns less than, equal to or greater than 0.
 */
static int
compare(
	const struct entry *left,
	const struct entry *right,
	const struct options *options)
{
	int order;

	/* -t, when both times are known: newer first, then by name. */
	if (options->time_sort && left->status_valid && right->status_valid) {
		if (left->status.st_mtime > right->status.st_mtime) {
			order = -1;
		} else if (left->status.st_mtime < right->status.st_mtime) {
			order = 1;
		} else {
			order = strcmp(left->name, right->name);
		}
	} else {
		order = strcmp(left->name, right->name);
	}

	/* -r turns it around. */
	if (options->reverse)
		return -order;

	/* Succeeded: the order. */
	return order;
}

/*
 * Writes a group of entries: in the long format, one to a line, or in
 * columns.  Returns 1, or 0 when an entry could not be listed.
 */
static int
print_entries(
	const char *path,
	struct entry *items,
	size_t count,
	const struct options *options)
{
	size_t index;
	int printed;

	/* The long format, with a total. */
	if (options->long_format) {
		printed = print_long_entries(path, items, count, options);
		if (!printed)
			return 0;
		return 1;
	}

	/* One name to a line, unless -C. */
	if (options->one || !options->columns) {
		for (index = 0; index < count; index++) {
			print_name(&items[index], options);
			putchar('\n');
		}

		/* Every name is listed. */
		return 1;
	}

	/* Succeeded: in columns. */
	print_columns(items, count, options);
	return 1;
}

/*
 * Writes a directory's entries in the long format after the total of their
 * blocks.  Returns 1, or 0 when an entry could not be listed.
 */
static int
print_long_entries(
	const char *path,
	struct entry *items,
	size_t count,
	const struct options *options)
{
	struct long_widths widths;
	unsigned long long blocks;
	char total[24];
	size_t index;
	int printed;
	int ok;

	/* The widths of the columns, and the blocks the entries take. */
	measure_long(items, count, options, &widths);
	blocks = 0;
	for (index = 0; index < count; index++) {
		if (items[index].status_valid && items[index].status.st_blocks > 0)
			blocks += (unsigned long long)items[index].status.st_blocks;
	}

	/* The total in kilobytes, or with -h in bytes with a unit. */
	if (options->human) {
		human_size((off_t)(blocks * 512ULL), total);
	} else {
		snprintf(total, sizeof(total), "%llu", (blocks + 1ULL) / 2ULL);
	}

	/* The line of the total. */
	printf("total %s\n", total);

	/* Each entry. */
	ok = 1;
	for (index = 0; index < count; index++) {
		printed = print_long(path, &items[index], options, &widths);
		if (!printed)
			ok = 0;
	}

	/* Reports an entry that could not be listed. */
	if (!ok)
		return 0;

	/* Succeeded. */
	return 1;
}

/*
 * Writes names in columns that fill 80 characters, down each column first
 * (-C).  Every column is as wide as the widest name, counting a -F mark
 * whether or not the name gets one, and two spaces.
 */
static void
print_columns(
	const struct entry *items,
	size_t count,
	const struct options *options)
{
	size_t width;
	size_t length;
	size_t columns;
	size_t rows;
	size_t row;
	size_t column;
	size_t index;
	size_t used;
	char mark;

	/* The width of a column. */
	width = 1;
	for (index = 0; index < count; index++) {
		length = strlen(items[index].name) + inode_width(&items[index], options);
		if (options->classify)
			length++;
		if (length > width)
			width = length;
	}

	/* Two spaces separate the columns. */
	width += 2U;

	/* As many columns as fit, and the rows they need. */
	columns = LS_COLUMNS_WIDTH / width;
	if (columns == 0)
		columns = 1;
	rows = (count + columns - 1U) / columns;

	/* Each row, taking every rows-th entry. */
	for (row = 0; row < rows; row++) {
		for (column = 0; column < columns; column++) {
			index = column * rows + row;
			if (index >= count)
				continue;

			/* The name, and what it took of the column. */
			print_name(&items[index], options);
			used = inode_width(&items[index], options) + strlen(items[index].name);
			mark = '\0';
			if (options->classify && items[index].status_valid)
				mark = suffix(items[index].status.st_mode);
			if (mark != '\0')
				used++;

			/* Spaces to the next column, unless this is the last name of the row. */
			if (column + 1U < columns && index + rows < count) {
				for (; used < width; used++)
					putchar(' ');
			}
		}

		/* The row ends. */
		putchar('\n');
	}
}

/* Finds the widths of the columns of the long format for a group of entries. */
static void
measure_long(
	const struct entry *items,
	size_t count,
	const struct options *options,
	struct long_widths *widths)
{
	char links[24];
	char size[32];
	char user_buffer[24];
	char group_buffer[24];
	const char *user;
	const char *group;
	size_t length;
	size_t index;

	/* Each entry whose status is known widens the columns it needs to. */
	memset(widths, 0, sizeof(*widths));
	for (index = 0; index < count; index++) {
		if (!items[index].status_valid)
			continue;

		/* The texts of the columns. */
		snprintf(links, sizeof(links), "%lu", (unsigned long)items[index].status.st_nlink);
		if (options->human) {
			human_size(items[index].status.st_size, size);
		} else {
			snprintf(size, sizeof(size), "%lld", (long long)items[index].status.st_size);
		}

		/* The owner and the group by name. */
		user = uid_name(items[index].status.st_uid, user_buffer);
		group = gid_name(items[index].status.st_gid, group_buffer);

		/* The link count. */
		length = strlen(links);
		if (length > widths->links)
			widths->links = length;

		/* The owner. */
		length = strlen(user);
		if (length > widths->user)
			widths->user = length;

		/* The group. */
		length = strlen(group);
		if (length > widths->group)
			widths->group = length;

		/* The size. */
		length = strlen(size);
		if (length > widths->size)
			widths->size = length;
	}
}

/*
 * Writes a size with -h: bytes below 1024, and otherwise the largest unit
 * that keeps it at least 1, with one decimal below 10 (1.5K, 12M).
 */
static void
human_size(
	off_t value,
	char out[16])
{
	static const char suffixes[] = "BKMGTPE";
	unsigned long long magnitude;
	unsigned long long scale;
	unsigned long long whole;
	unsigned long long remainder;
	unsigned long long tenth;
	unsigned unit;

	/* A negative size is written as it is. */
	if (value < 0) {
		snprintf(out, 16, "%lld", (long long)value);
		return;
	}

	/* The largest unit the size reaches. */
	magnitude = (unsigned long long)value;
	unit = 0;
	scale = 1;
	while (unit + 1U < sizeof(suffixes) - 1U && magnitude >= scale * 1024ULL) {
		scale *= 1024ULL;
		unit++;
	}

	/* Bytes. */
	if (unit == 0) {
		snprintf(out, 16, "%llu", magnitude);
		return;
	}

	/* One rounded decimal below 10, and a rounded whole number from 10 on. */
	whole = magnitude / scale;
	remainder = magnitude % scale;
	if (whole < 10U) {
		tenth = (remainder * 10ULL + scale / 2ULL) / scale;
		if (tenth == 10U) {
			whole++;
			tenth = 0;
		}

		/* The whole number, the decimal and the unit. */
		snprintf(out, 16, "%llu.%llu%c", whole, tenth, suffixes[unit]);
	} else {
		whole = (magnitude + scale / 2ULL) / scale;
		snprintf(out, 16, "%llu%c", whole, suffixes[unit]);
	}
}

/* Returns the name of a user, or the number when it has no name. */
static const char *
uid_name(
	uid_t id,
	char out[24])
{
	struct passwd record;
	struct passwd *found;
	char buffer[512];
	int error;

	/* The name from the user database. */
	found = NULL;
	error = getpwuid_r(id, &record, buffer, sizeof(buffer), &found);
	if (error == 0 && found != NULL && found->pw_name != NULL) {
		snprintf(out, 24, "%s", found->pw_name);
		return out;
	}

	/* Succeeded: the number. */
	snprintf(out, 24, "%u", (unsigned)id);
	return out;
}

/* Returns the name of a group, or the number when it has no name. */
static const char *
gid_name(
	gid_t id,
	char out[24])
{
	struct group record;
	struct group *found;
	char buffer[512];
	int error;

	/* The name from the group database. */
	found = NULL;
	error = getgrgid_r(id, &record, buffer, sizeof(buffer), &found);
	if (error == 0 && found != NULL && found->gr_name != NULL) {
		snprintf(out, 24, "%s", found->gr_name);
		return out;
	}

	/* Succeeded: the number. */
	snprintf(out, 24, "%u", (unsigned)id);
	return out;
}

/*
 * Writes one entry in the long format: mode, links, owner, group, size,
 * time and name, and where a symbolic link points.  Returns 1, or 0 after
 * a message when the status is unknown.
 */
static int
print_long(
	const char *directory,
	const struct entry *item,
	const struct options *options,
	const struct long_widths *widths)
{
	char target[LS_PATH_CAPACITY];
	char path[LS_PATH_CAPACITY];
	char mode[11];
	char size[32];
	char when[32];
	char user_buffer[24];
	char group_buffer[24];
	const char *user;
	const char *group;
	ssize_t length;
	int joined;
	int link;

	/* Nothing is known of an entry without its status. */
	if (!item->status_valid) {
		command_error("ls", item->name);
		return 0;
	}

	/* The texts of the columns. */
	mode_text(item->status.st_mode, mode);
	if (options->human) {
		human_size(item->status.st_size, size);
	} else {
		snprintf(size, sizeof(size), "%lld", (long long)item->status.st_size);
	}

	/* The time, and the owner and the group by name. */
	ls_time(item->status.st_mtime, when);
	user = uid_name(item->status.st_uid, user_buffer);
	group = gid_name(item->status.st_gid, group_buffer);

	/* -i: the file serial number first. */
	if (options->inode)
		printf("%lu ", (unsigned long)item->status.st_ino);

	/* The columns and the name. */
	printf("%s %*lu %-*s %-*s %*s %s %s",
	       mode,
	       (int)widths->links,
	       (unsigned long)item->status.st_nlink,
	       (int)widths->user,
	       user,
	       (int)widths->group,
	       group,
	       (int)widths->size,
	       size,
	       when,
	       item->name);

	/* A symbolic link: where it points, when that can be read. */
	link = S_ISLNK(item->status.st_mode);
	if (link) {
		joined = join_path(directory, item->name, path, sizeof(path));
		length = -1;
		if (joined)
			length = readlink(path, target, sizeof(target) - 1U);
		if (length >= 0) {
			target[length] = '\0';
			printf(" -> %s", target);
		}
	}

	/* Succeeded: the line ends. */
	putchar('\n');
	return 1;
}

/* Writes the ten characters of a mode: the type, then rwx for each class with s and t. */
static void
mode_text(
	mode_t mode,
	char out[11])
{
	static const mode_t bits[] = {
		S_IRUSR, S_IWUSR, S_IXUSR,
		S_IRGRP, S_IWGRP, S_IXGRP,
		S_IROTH, S_IWOTH, S_IXOTH
	};
	static const char letters[] = "rwx";
	unsigned index;

	/* The type, then a letter or a dash for each permission. */
	out[0] = type_char(mode);
	for (index = 0; index < 9; index++) {
		out[index + 1] = '-';
		if ((mode & bits[index]) != 0)
			out[index + 1] = letters[index % 3];
	}

	/* Set-user-ID in the owner's execute place: s over x, S without it. */
	if ((mode & S_ISUID) != 0) {
		out[3] = 'S';
		if ((mode & S_IXUSR) != 0)
			out[3] = 's';
	}

	/* Set-group-ID in the group's execute place. */
	if ((mode & S_ISGID) != 0) {
		out[6] = 'S';
		if ((mode & S_IXGRP) != 0)
			out[6] = 's';
	}

	/* The sticky bit in the others' execute place: t over x, T without it. */
	if ((mode & S_ISVTX) != 0) {
		out[9] = 'T';
		if ((mode & S_IXOTH) != 0)
			out[9] = 't';
	}

	/* The text ends. */
	out[10] = '\0';
}

/* Returns the letter of the long format for the type of a file. */
static char
type_char(
	mode_t mode)
{
	/* A directory, a character or block device, a FIFO, a link, a socket. */
	switch (mode & S_IFMT) {
	case S_IFDIR:
		return 'd';
	case S_IFCHR:
		return 'c';
	case S_IFBLK:
		return 'b';
	case S_IFIFO:
		return 'p';
	case S_IFLNK:
		return 'l';
	case S_IFSOCK:
		return 's';
	default:
		break;
	}

	/* A regular file. */
	return '-';
}

/*
 * Writes the time of the long format in UTC: month, day and hour:minute
 * for a time within the last six months (and the next hour), and month,
 * day and year for any other.
 */
static void
ls_time(
	time_t value,
	char out[32])
{
	static const char *const month_names[] = {
		"Jan", "Feb", "Mar", "Apr", "May", "Jun",
		"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
	};
	long long days;
	long long seconds;
	long long year;
	time_t now;
	int month;
	int length;
	int recent;

	/* The day since the epoch and the second in the day, the second never negative. */
	days = value / 86400;
	seconds = value % 86400;
	if (seconds < 0) {
		seconds += 86400;
		days--;
	}

	/* The year, counting whole years forward or back from 1970. */
	year = 1970;
	for (;;) {
		length = days_in_year(year);
		if (days < length)
			break;
		days -= length;
		year++;
	}
	while (days < 0) {
		year--;
		length = days_in_year(year);
		days += length;
	}

	/* The month, counting whole months into the year. */
	month = 0;
	while (month < 11) {
		length = days_in_month(month, year);
		if (days < length)
			break;
		days -= length;
		month++;
	}

	/* A recent time shows the hour and minute, and any other the year. */
	now = time(NULL);
	recent = 1;
	if (now != (time_t)-1 && (value < now - LS_RECENT_SECONDS || value > now + 3600))
		recent = 0;
	if (recent) {
		snprintf(out, 32, "%s %2d %02lld:%02lld", month_names[month], (int)days + 1, seconds / 3600, (seconds / 60) % 60);
	} else {
		snprintf(out, 32, "%s %2d  %4lld", month_names[month], (int)days + 1, year);
	}
}

/* Returns the number of days of a year of the Gregorian calendar. */
static int
days_in_year(
	long long year)
{
	/* A year divisible by 400 is a leap year; by 100 otherwise not; by 4 otherwise it is. */
	if (year % 400 == 0)
		return 366;
	if (year % 100 == 0)
		return 365;
	if (year % 4 == 0)
		return 366;

	/* Any other year. */
	return 365;
}

/* Returns the number of days of a month (0 for January) in a year. */
static int
days_in_month(
	int month,
	long long year)
{
	static const int lengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	int year_length;

	/* February has a 29th day in a leap year. */
	if (month == 1) {
		year_length = days_in_year(year);
		if (year_length == 366)
			return 29;
	}

	/* Any other month, or February in another year. */
	return lengths[month];
}

/* Writes a name, after its serial number with -i and before its mark with -F. */
static void
print_name(
	const struct entry *item,
	const struct options *options)
{
	char mark;

	/* -i: the file serial number before the name. */
	if (options->inode && item->status_valid)
		printf("%lu ", (unsigned long)item->status.st_ino);
	printf("%s", item->name);

	/* -F: the mark of the type, when it has one. */
	if (options->classify && item->status_valid) {
		mark = suffix(item->status.st_mode);
		if (mark != '\0')
			putchar(mark);
	}
}

/* Returns the width -i adds before a name: the serial number and a space. */
static size_t
inode_width(
	const struct entry *item,
	const struct options *options)
{
	char digits[32];
	int length;

	/* Nothing without -i or without the status. */
	if (!options->inode || !item->status_valid)
		return 0;

	/* Succeeded: the digits and the space. */
	length = snprintf(digits, sizeof(digits), "%lu ", (unsigned long)item->status.st_ino);
	return (size_t)length;
}

/* Returns the -F mark for the type of a file, or 0 for none. */
static char
suffix(
	mode_t mode)
{
	/* A directory, a link, a FIFO, a socket. */
	switch (mode & S_IFMT) {
	case S_IFDIR:
		return '/';
	case S_IFLNK:
		return '@';
	case S_IFIFO:
		return '|';
	case S_IFSOCK:
		return '=';
	default:
		break;
	}

	/* An executable file. */
	if ((mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0)
		return '*';

	/* Anything else has none. */
	return '\0';
}

/* Frees entries and their names. */
static void
free_entries(
	struct entry *items,
	size_t count)
{
	size_t index;

	/* Each name, then the array. */
	for (index = 0; index < count; index++)
		free(items[index].name);
	free(items);
}
