/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/sh/builtins.h"

#include <zedbsd/console.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define COPY_BUFFER_SIZE 512U
#define LS_INITIAL_CAPACITY 16U
#define PATH_BUFFER_SIZE 256U

struct ls_entry {
	char name[256];
	uint8_t type;
	struct stat status;
	int status_valid;
};

static int
write_all(int descriptor, const void *buffer, size_t length)
{
	const unsigned char *bytes = buffer;
	while (length != 0) {
		ssize_t count = write(descriptor, bytes, length);
		if (count <= 0)
			return 0;
		bytes += count;
		length -= (size_t)count;
	}
	return 1;
}

static const char *
path_basename(const char *path)
{
	const char *slash = strrchr(path, '/');
	return slash == NULL ? path : slash + 1;
}

static int
join_path(const char *directory, const char *name, char *result,
	  size_t capacity)
{
	size_t directory_length = strlen(directory);
	size_t name_length = strlen(name);
	int slash = directory_length != 0 &&
	    directory[directory_length - 1U] != '/';
	if (directory_length + (size_t)slash + name_length + 1U > capacity)
		return 0;
	memcpy(result, directory, directory_length);
	if (slash)
		result[directory_length++] = '/';
	memcpy(result + directory_length, name, name_length + 1U);
	return 1;
}

static int
builtin_echo(int argc, char **argv)
{
	int index;
	for (index = 1; index < argc; index++)
		printf("%s%s", index == 1 ? "" : " ", argv[index]);
	putchar('\n');
	return 1;
}

static int
builtin_pwd(int argc, char **argv)
{
	char path[PATH_BUFFER_SIZE];
	(void)argv;
	if (argc != 1) {
		fprintf(stderr, "usage: pwd\n");
		return 0;
	}
	if (getcwd(path, sizeof(path)) == NULL) {
		fprintf(stderr, "pwd: %s\n", strerror(errno));
		return 0;
	}
	puts(path);
	return 1;
}

static int
builtin_cd(int argc, char **argv)
{
	const char *path;
	if (argc > 2) {
		fprintf(stderr, "usage: cd [DIRECTORY]\n");
		return 0;
	}
	path = argc == 2 ? argv[1] : getenv("HOME");
	if (path == NULL || path[0] == '\0')
		path = "/";
	if (chdir(path) != 0) {
		fprintf(stderr, "cd: %s: %s\n", path, strerror(errno));
		return 0;
	}
	return 1;
}

static int
builtin_cat(int argc, char **argv)
{
	int argument;
	if (argc < 2) {
		fprintf(stderr, "usage: cat FILE...\n");
		return 0;
	}
	for (argument = 1; argument < argc; argument++) {
		unsigned char buffer[COPY_BUFFER_SIZE];
		ssize_t count;
		int descriptor = open(argv[argument], O_RDONLY);
		if (descriptor < 0) {
			fprintf(stderr, "cat: %s: %s\n", argv[argument],
			    strerror(errno));
			return 0;
		}
		while ((count = read(descriptor, buffer, sizeof(buffer))) > 0) {
			if (!write_all(1, buffer, (size_t)count)) {
				fprintf(stderr, "cat: write: %s\n", strerror(errno));
				(void)close(descriptor);
				return 0;
			}
		}
		if (count < 0) {
			fprintf(stderr, "cat: %s: %s\n", argv[argument],
			    strerror(errno));
			(void)close(descriptor);
			return 0;
		}
		if (close(descriptor) != 0) {
			fprintf(stderr, "cat: %s: %s\n", argv[argument],
			    strerror(errno));
			return 0;
		}
	}
	return 1;
}

static void
sort_entries(struct ls_entry *entries, size_t count)
{
	size_t index;
	for (index = 1; index < count; index++) {
		struct ls_entry current = entries[index];
		size_t position = index;
		while (position != 0 &&
		    strcmp(entries[position - 1U].name, current.name) > 0) {
			entries[position] = entries[position - 1U];
			position--;
		}
		entries[position] = current;
	}
}

static char
type_character(mode_t mode)
{
	if (S_ISDIR(mode)) return 'd';
	if (S_ISCHR(mode)) return 'c';
	if (S_ISBLK(mode)) return 'b';
	if (S_ISFIFO(mode)) return 'p';
	if (S_ISLNK(mode)) return 'l';
	if (S_ISSOCK(mode)) return 's';
	return '-';
}

static void
mode_text(mode_t mode, char result[11])
{
	static const mode_t bits[9] = {
		S_IRUSR, S_IWUSR, S_IXUSR, S_IRGRP, S_IWGRP, S_IXGRP,
		S_IROTH, S_IWOTH, S_IXOTH
	};
	static const char letters[3] = { 'r', 'w', 'x' };
	unsigned index;
	result[0] = type_character(mode);
	for (index = 0; index < 9; index++)
		result[index + 1U] = mode & bits[index] ? letters[index % 3U] : '-';
	result[10] = '\0';
}

static int
load_directory(const char *path, int long_format, struct ls_entry **result,
	       size_t *result_count)
{
	DIR *directory = opendir(path);
	struct ls_entry *entries;
	struct dirent *entry;
	size_t count = 0, capacity = LS_INITIAL_CAPACITY;
	if (directory == NULL)
		return 0;
	entries = malloc(capacity * sizeof(*entries));
	if (entries == NULL) {
		(void)closedir(directory);
		return 0;
	}
	while ((entry = readdir(directory)) != NULL) {
		struct ls_entry *item;
		if (count == capacity) {
			struct ls_entry *larger;
			if (capacity > (size_t)-1 / 2U / sizeof(*entries)) {
				free(entries);
				(void)closedir(directory);
				errno = ENOMEM;
				return 0;
			}
			capacity *= 2U;
			larger = realloc(entries, capacity * sizeof(*entries));
			if (larger == NULL) {
				free(entries);
				(void)closedir(directory);
				return 0;
			}
			entries = larger;
		}
		item = &entries[count++];
		strncpy(item->name, entry->d_name, sizeof(item->name) - 1U);
		item->name[sizeof(item->name) - 1U] = '\0';
		item->type = entry->d_type;
		item->status_valid = 0;
		if (long_format) {
			char child[PATH_BUFFER_SIZE];
			if (join_path(path, item->name, child, sizeof(child)) &&
			    stat(child, &item->status) == 0)
				item->status_valid = 1;
		}
	}
	if (closedir(directory) != 0) {
		free(entries);
		return 0;
	}
	sort_entries(entries, count);
	*result = entries;
	*result_count = count;
	return 1;
}

static int
entry_is_directory(const struct ls_entry *entry)
{
	return entry->status_valid ? S_ISDIR(entry->status.st_mode) :
	    entry->type == DT_DIR;
}

static size_t
entry_display_length(const struct ls_entry *entry)
{
	return strlen(entry->name) + (entry_is_directory(entry) ? 1U : 0U);
}

static void
print_entry_name(const struct ls_entry *entry)
{
	printf("%s%s", entry->name, entry_is_directory(entry) ? "/" : "");
}

static void
print_long_entries(const struct ls_entry *entries, size_t count)
{
	size_t index;
	for (index = 0; index < count; index++) {
		char mode[11];
		if (entries[index].status_valid) {
			mode_text(entries[index].status.st_mode, mode);
			printf("%s %10lld ", mode,
			    (long long)entries[index].status.st_size);
		} else {
			strcpy(mode, "??????????");
			printf("%s %10s ", mode, "?");
		}
		print_entry_name(&entries[index]);
		putchar('\n');
	}
}

static void
print_column_entries(const struct ls_entry *entries, size_t count)
{
	struct zedbsd_console_size size = { 0, 80 };
	size_t maximum = 0, column_width, columns, rows, row, column;
	if (count == 0)
		return;
	(void)ioctl(1, ZEDBSD_CONSOLE_GET_SIZE, &size);
	if (size.columns == 0)
		size.columns = 80;
	for (row = 0; row < count; row++) {
		size_t length = entry_display_length(&entries[row]);
		if (length > maximum)
			maximum = length;
	}
	column_width = maximum + 2U;
	columns = size.columns / column_width;
	if (columns == 0) columns = 1;
	if (columns > count) columns = count;
	rows = (count + columns - 1U) / columns;
	for (row = 0; row < rows; row++) {
		for (column = 0; column < columns; column++) {
			size_t index = row + column * rows;
			size_t next = index + rows;
			size_t length, spaces;
			if (index >= count)
				continue;
			print_entry_name(&entries[index]);
			length = entry_display_length(&entries[index]);
			if (column + 1U >= columns || next >= count)
				continue;
			spaces = column_width - length;
			while (spaces-- != 0)
				putchar(' ');
		}
		putchar('\n');
	}
}

static int
builtin_ls(int argc, char **argv)
{
	const char *path = ".";
	int long_format = 0, path_set = 0, argument;
	struct stat status;
	struct ls_entry *entries;
	size_t count;
	for (argument = 1; argument < argc; argument++) {
		if (!strcmp(argv[argument], "-l"))
			long_format = 1;
		else if (!path_set) {
			path = argv[argument];
			path_set = 1;
		} else {
			fprintf(stderr, "usage: ls [-l] [PATH]\n");
			return 0;
		}
	}
	if (stat(path, &status) != 0) {
		fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
		return 0;
	}
	if (!S_ISDIR(status.st_mode)) {
		struct ls_entry single;
		memset(&single, 0, sizeof(single));
		strncpy(single.name, path_basename(path), sizeof(single.name) - 1U);
		single.status = status;
		single.status_valid = 1;
		if (long_format)
			print_long_entries(&single, 1);
		else {
			print_entry_name(&single);
			putchar('\n');
		}
		return 1;
	}
	if (!load_directory(path, long_format, &entries, &count)) {
		fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
		return 0;
	}
	if (long_format)
		print_long_entries(entries, count);
	else
		print_column_entries(entries, count);
	free(entries);
	return 1;
}

static int
builtin_cp(int argc, char **argv)
{
	unsigned char buffer[COPY_BUFFER_SIZE];
	char destination_path[PATH_BUFFER_SIZE];
	const char *destination;
	struct stat source_status, destination_status;
	int source = -1, target = -1, success = 0, destination_exists = 0;
	ssize_t count;
	if (argc != 3) {
		fprintf(stderr, "usage: cp SOURCE DESTINATION\n");
		return 0;
	}
	source = open(argv[1], O_RDONLY);
	if (source < 0 || fstat(source, &source_status) != 0) {
		fprintf(stderr, "cp: %s: %s\n", argv[1], strerror(errno));
		if (source >= 0) (void)close(source);
		return 0;
	}
	if (!S_ISREG(source_status.st_mode)) {
		fprintf(stderr, "cp: %s: not a regular file\n", argv[1]);
		(void)close(source);
		return 0;
	}
	destination = argv[2];
	if (stat(destination, &destination_status) == 0) {
		destination_exists = 1;
		if (S_ISDIR(destination_status.st_mode)) {
			if (!join_path(destination, path_basename(argv[1]),
			    destination_path, sizeof(destination_path))) {
				fprintf(stderr, "cp: destination path is too long\n");
				(void)close(source);
				return 0;
			}
			destination = destination_path;
			destination_exists = stat(destination,
			    &destination_status) == 0;
		}
	} else if (errno != ENOENT) {
		fprintf(stderr, "cp: %s: %s\n", destination, strerror(errno));
		(void)close(source);
		return 0;
	}
	if (destination_exists && source_status.st_dev == destination_status.st_dev &&
	    source_status.st_ino == destination_status.st_ino) {
		fprintf(stderr, "cp: source and destination are the same file\n");
		(void)close(source);
		return 0;
	}
	/* Without rename/unlink syscalls a failed copy may leave a partial file. */
	target = open(destination, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (target < 0) {
		fprintf(stderr, "cp: %s: %s\n", destination, strerror(errno));
		(void)close(source);
		return 0;
	}
	while ((count = read(source, buffer, sizeof(buffer))) > 0) {
		if (!write_all(target, buffer, (size_t)count)) {
			fprintf(stderr, "cp: %s: %s\n", destination, strerror(errno));
			goto done;
		}
	}
	if (count < 0) {
		fprintf(stderr, "cp: %s: %s\n", argv[1], strerror(errno));
		goto done;
	}
	success = 1;
done:
	if (close(target) != 0) {
		fprintf(stderr, "cp: %s: %s\n", destination, strerror(errno));
		success = 0;
	}
	if (close(source) != 0) {
		fprintf(stderr, "cp: %s: %s\n", argv[1], strerror(errno));
		success = 0;
	}
	return success;
}

static const char *
type_name(mode_t mode)
{
	if (S_ISREG(mode)) return "regular";
	if (S_ISDIR(mode)) return "directory";
	if (S_ISCHR(mode)) return "character";
	if (S_ISBLK(mode)) return "block";
	if (S_ISFIFO(mode)) return "fifo";
	if (S_ISLNK(mode)) return "symlink";
	if (S_ISSOCK(mode)) return "socket";
	return "unknown";
}

static int
builtin_stat(int argc, char **argv)
{
	int argument;
	if (argc < 2) {
		fprintf(stderr, "usage: stat PATH...\n");
		return 0;
	}
	for (argument = 1; argument < argc; argument++) {
		struct stat status;
		if (stat(argv[argument], &status) != 0) {
			fprintf(stderr, "stat: %s: %s\n", argv[argument],
			    strerror(errno));
			return 0;
		}
		printf("%s: type=%s mode=%x dev=%u ino=%u links=%u "
		    "uid=%u gid=%u size=%lld\n", argv[argument],
		    type_name(status.st_mode), (unsigned)status.st_mode,
		    (unsigned)status.st_dev, (unsigned)status.st_ino,
		    (unsigned)status.st_nlink, (unsigned)status.st_uid,
		    (unsigned)status.st_gid, (long long)status.st_size);
	}
	return 1;
}

static int
builtin_touch(int argc, char **argv)
{
	int argument;
	if (argc < 2) {
		fprintf(stderr, "usage: touch FILE...\n");
		return 0;
	}
	for (argument = 1; argument < argc; argument++) {
		int descriptor = open(argv[argument], O_WRONLY | O_CREAT, 0666);
		if (descriptor < 0 || close(descriptor) != 0) {
			fprintf(stderr, "touch: %s: %s\n", argv[argument],
			    strerror(errno));
			if (descriptor >= 0) (void)close(descriptor);
			return 0;
		}
	}
	return 1;
}

int
sh_builtin_dispatch(int argc, char **argv, int *handled)
{
	*handled = 1;
	if (!strcmp(argv[0], "echo")) return builtin_echo(argc, argv);
	if (!strcmp(argv[0], "pwd")) return builtin_pwd(argc, argv);
	if (!strcmp(argv[0], "cd")) return builtin_cd(argc, argv);
	if (!strcmp(argv[0], "cat")) return builtin_cat(argc, argv);
	if (!strcmp(argv[0], "ls")) return builtin_ls(argc, argv);
	if (!strcmp(argv[0], "cp")) return builtin_cp(argc, argv);
	if (!strcmp(argv[0], "stat")) return builtin_stat(argc, argv);
	if (!strcmp(argv[0], "touch")) return builtin_touch(argc, argv);
	if (!strcmp(argv[0], "clear")) {
		if (argc != 1) {
			fprintf(stderr, "usage: clear\n");
			return 0;
		}
		return ioctl(1, ZEDBSD_CONSOLE_CLEAR) == 0;
	}
	if (!strcmp(argv[0], "true")) return 1;
	if (!strcmp(argv[0], "false")) return 0;
	*handled = 0;
	return 0;
}
