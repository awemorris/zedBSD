/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD pax userland command.
 */

#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <inttypes.h>
#include <limits.h>
#include <regex.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define BLOCK_SIZE 512U

struct tar_header {
	char name[100];
	char mode[8];
	char uid[8];
	char gid[8];
	char size[12];
	char mtime[12];
	char checksum[8];
	char type;
	char linkname[100];
	char magic[6];
	char version[2];
	char uname[32];
	char gname[32];
	char devmajor[8];
	char devminor[8];
	char prefix[155];
	char padding[12];
};

struct hard_link {
	dev_t device;
	ino_t inode;
	char *name;
	struct hard_link *next;
};

struct substitution {
	regex_t expression;
	char *replacement;
	int global;
	struct substitution *next;
};

static FILE *archive_file;
static int exit_status;
static int verbose;
static int preserve_mode = 1;
static int preserve_time = 1;
static struct hard_link *links;
static struct substitution *substitutions;

static int add_substitution(const char *argument);
static void usage(void);
static int copy_operands(char **operands, int count, const char *destination);
static int write_operands(char **operands, int count);
static const char *portable_name(const char *name);
static int write_tree(const char *filesystem_name, const char *stored_name);
static void warn_path(const char *operation, const char *path);
static int write_member(const char *filesystem_name, const char *stored_name, const struct stat *status);
static const char *known_link(const struct stat *status);
static int fill_header(struct tar_header *header, const char *name, const struct stat *status, char type, const char *linkname, off_t size);
static int split_header_name(struct tar_header *header, const char *name);
static int format_octal(char *field, size_t length, uint64_t value);
static unsigned header_checksum(const struct tar_header *header);
static int write_all(FILE *file, const void *buffer, size_t length);
static int copy_stream(FILE *input, FILE *output, uint64_t length);
static int read_all(FILE *file, void *buffer, size_t length);
static int pad_output(uint64_t length);
static int remember_link(const struct stat *status, const char *name);
static int finish_archive(void);
static int read_archive(int extract, char **patterns, int pattern_count);
static int zero_block(const unsigned char *block);
static int parse_octal(const char *field, size_t length, uint64_t *value);
static char *archive_name(const struct tar_header *header);
static char *transform_name(const char *name);
static char *replace_once(const char *input, const struct substitution *rule, int *matched);
static int matches_patterns(const char *name, char **patterns, int pattern_count);
static int skip_payload(uint64_t length);
static int safe_path(const char *path);
static int extract_regular(const char *path, uint64_t size, mode_t mode, time_t mtime);
static int make_parents(const char *path);
static int remove_existing(const char *path, int directory);
static void apply_metadata(const char *path, mode_t mode, time_t modification, int symlink);
static int consume_padding(uint64_t size);
static int position_for_append(FILE *file);

/*
 * Runs the pax command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	int read_mode;
	int write_mode;
	int append_mode;
	int option;
	const char *archive_name_value;
	const char *format;
	const char *open_mode;

	read_mode = 0;
	write_mode = 0;
	append_mode = 0;
	archive_name_value = NULL;
	format = "ustar";

	/* Parse each command-line option. */
	while ((option = getopt(argc, argv, "rwaf:x:s:vp:")) != -1) {
		/* Dispatch the selected command-line option. */
		switch (option) {
		case 'r':
			read_mode = 1;
			break;
		case 'w':
			write_mode = 1;
			break;
		case 'a':
			append_mode = 1;
			break;
		case 'f':
			archive_name_value = optarg;
			break;
		case 'x':
			format = optarg;
			break;
		case 's':
			/* Handles a failed add substitution operation. */
			if (add_substitution(optarg) != 0) {
				fprintf(stderr,
					"pax: invalid replacement: %s\n",
					optarg);

				/* Reports operation failure. */
				return 2;
			}
			break;
		case 'v':
			verbose = 1;
			break;
		case 'p':
			preserve_mode = strchr(optarg, 'p') != NULL ||
					strchr(optarg, 'e') != NULL;
			preserve_time = strchr(optarg, 'm') != NULL ||
					strchr(optarg, 'e') != NULL;
			break;
		default:
			usage();

			/* Reports operation failure. */
			return 2;
		}
	}

	/* Handles the append mode condition. */
	if (append_mode && !write_mode) {
		fprintf(stderr, "pax: -a requires -w\n");

		/* Reports operation failure. */
		return 2;
	}

	/* Handles the read mode condition. */
	if (read_mode && write_mode) {
		/* Validates the command-line arguments. */
		if (archive_name_value != NULL || append_mode ||
		    optind + 1 >= argc) {
			usage();

			/* Reports operation failure. */
			return 2;
		}

		/* Computes the function result. */
		function_result = copy_operands(argv + optind, argc - optind - 1,
				     argv[argc - 1]) == 0
			   ? exit_status
			   : 1;

		/* Returns the computed result. */
		return function_result;
	}

	/* Selects the matching value. */
	if (strcmp(format, "ustar") != 0 && strcmp(format, "pax") != 0) {
		fprintf(stderr, "pax: unsupported archive format: %s\n",
			format);

		/* Reports operation failure. */
		return 2;
	}

	/* Handles the write mode condition. */
	if (write_mode) {
		/* Handles the archive name value availability. */
		if (archive_name_value == NULL ||
		    !strcmp(archive_name_value, "-")) {
			/* Handles the append mode condition. */
			if (append_mode) {
				fprintf(
				    stderr,
				    "pax: cannot append to standard output\n");

				/* Reports operation failure. */
				return 2;
			}
			archive_file = stdout;
		} else {
			open_mode = append_mode ? "r+b" : "wb";
			archive_file = fopen(archive_name_value, open_mode);

			/* Handles the archive file availability. */
			if (archive_file == NULL) {
				warn_path("open archive", archive_name_value);

				/* Reports operation failure. */
				return 1;
			}

			/* Handles a failed position for append operation. */
			if (append_mode &&
			    position_for_append(archive_file) != 0) {
				fprintf(stderr,
					"pax: invalid archive for append: %s\n",
					archive_name_value);
				fclose(archive_file);

				/* Reports operation failure. */
				return 1;
			}
		}

		/* Validates the command-line arguments. */
		if (write_operands(argv + optind, argc - optind) != 0)
			exit_status = 1;

		/* Handles a failed fclose operation. */
		if (archive_file != stdout && fclose(archive_file) != 0)
			exit_status = 1;

		/* Returns the computed result. */
		return exit_status;
	}

	/* Handles the archive name value availability. */
	if (archive_name_value == NULL || !strcmp(archive_name_value, "-")) {
		archive_file = stdin;
	} else {
		archive_file = fopen(archive_name_value, "rb");

		/* Handles the archive file availability. */
		if (archive_file == NULL) {
			warn_path("open archive", archive_name_value);

			/* Reports operation failure. */
			return 1;
		}
	}

	/* Validates the command-line arguments. */
	if (read_archive(read_mode, argv + optind, argc - optind) != 0) {
		fprintf(stderr, "pax: archive read failed\n");
		exit_status = 1;
	}

	/* Handles a failed fclose operation. */
	if (archive_file != stdin && fclose(archive_file) != 0)
		exit_status = 1;

	/* Returns the computed result. */
	return exit_status;
}

/* Supports the add substitution operation. */
static int
add_substitution(
	const char *argument)
{
	char delimiter;
	const char *expression_end;
	const char *replacement_end;
	char *expression;
	struct substitution *rule;

	/* Handles the argument condition. */
	if (argument[0] == '\0')
		return -1;
	delimiter = argument[0];
	expression_end = strchr(argument + 1, delimiter);

	/* Handles the expression end availability. */
	if (expression_end == NULL)
		return -1;
	replacement_end = strchr(expression_end + 1, delimiter);

	/* Handles the replacement end availability. */
	if (replacement_end == NULL)
		return -1;
	expression =
	    strndup(argument + 1, (size_t)(expression_end - argument - 1));
	rule = calloc(1, sizeof(*rule));

	/* Handles the expression availability. */
	if (expression == NULL || rule == NULL) {
		free(expression);
		free(rule);

		/* Reports operation failure. */
		return -1;
	}
	rule->replacement = strndup(
	    expression_end + 1, (size_t)(replacement_end - expression_end - 1));

	/* Handles a failed regcomp operation. */
	if (rule->replacement == NULL ||
	    regcomp(&rule->expression, expression, 0) != 0) {
		free(expression);
		free(rule->replacement);
		free(rule);

		/* Reports operation failure. */
		return -1;
	}
	free(expression);

	/* Process each element required by the operation. */
	for (replacement_end++; *replacement_end != '\0'; replacement_end++) {
		/* Handles the replacement end condition. */
		if (*replacement_end == 'g')
			rule->global = 1;
		else if (*replacement_end != 'p') {
			regfree(&rule->expression);
			free(rule->replacement);
			free(rule);

			/* Reports operation failure. */
			return -1;
		}
	}
	rule->next = substitutions;
	substitutions = rule;

	/* Reports successful completion. */
	return 0;
}

/* Supports the usage operation. */
static void
usage(
	void)
{
	fprintf(stderr,
		"usage: pax [-rv] [-f archive] [-s replacement] [pattern ...]\n"
		"       pax -w [-av] [-f archive] [-x ustar] [file ...]\n"
		"       pax -rw [-v] file ... directory\n");
}

/* Supports the copy operands operation. */
static int
copy_operands(
	char **operands,
	int count,
	const char *destination)
{
	FILE *temporary;
	int saved_directory;
	struct stat status;

	/* Checks the remaining item count. */
	if (count == 0) {
		fprintf(stderr,
			"pax: copy mode requires at least one source\n");

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed stat operation. */
	if (stat(destination, &status) != 0 || !S_ISDIR(status.st_mode)) {
		fprintf(stderr,
			"pax: copy destination is not a directory: %s\n",
			destination);

		/* Reports operation failure. */
		return -1;
	}
	temporary = tmpfile();

	/* Handles the temporary availability. */
	if (temporary == NULL)
		return -1;
	archive_file = temporary;

	/* Handles a failed write operands operation. */
	if (write_operands(operands, count) != 0 ||
	    fseeko(temporary, 0, SEEK_SET) != 0) {
		fclose(temporary);

		/* Reports operation failure. */
		return -1;
	}
	saved_directory = open(".", O_RDONLY | O_DIRECTORY);

	/* Handles a failed chdir operation. */
	if (saved_directory < 0 || chdir(destination) != 0) {
		/* Handles the saved directory condition. */
		if (saved_directory >= 0)
			close(saved_directory);
		fclose(temporary);

		/* Reports operation failure. */
		return -1;
	}
	(void)read_archive(1, NULL, 0);

	/* Handles a failed fchdir operation. */
	if (fchdir(saved_directory) != 0) {
		fprintf(stderr, "pax: cannot return to original directory\n");
		exit_status = 1;
	}
	close(saved_directory);
	fclose(temporary);

	/* Returns the computed result. */
	return exit_status ? -1 : 0;
}

/* Supports the write operands operation. */
static int
write_operands(
	char **operands,
	int count)
{
	int function_result;
	const char *stored;
	int index;

	/* Process each remaining element. */
	for (index = 0; index < count; index++) {
		stored = portable_name(operands[index]);

		/* Handles a failed write tree operation. */
		if (write_tree(operands[index], stored) != 0)
			exit_status = 1;
	}

	/* Obtains the finish archive result. */
	function_result = finish_archive();

	/* Returns the computed result. */
	return function_result;
}

/* Supports the portable name operation. */
static const char *
portable_name(
	const char *name)
{
	/* Continue while the operation condition remains true. */
	while (name[0] == '/' || (name[0] == '.' && name[1] == '/'))
		name += name[0] == '/' ? 1 : 2;

	/* Returns the computed result. */
	return *name == '\0' ? "." : name;
}

/* Supports the write tree operation. */
static int
write_tree(
	const char *filesystem_name,
	const char *stored_name)
{
	char *child_fs;
	char *child_stored;
	size_t fs_length;
	size_t stored_length;
	struct stat status;
	DIR *directory;
	struct dirent *entry;

	/* Handles a failed lstat operation. */
	if (lstat(filesystem_name, &status) != 0) {
		warn_path("stat", filesystem_name);

		/* Reports operation failure. */
		return -1;
	}
	(void)write_member(filesystem_name, stored_name, &status);

	/* Handles a failed S ISDIR operation. */
	if (!S_ISDIR(status.st_mode))
		return 0;
	directory = opendir(filesystem_name);

	/* Handles the directory availability. */
	if (directory == NULL) {
		warn_path("open directory", filesystem_name);

		/* Reports operation failure. */
		return -1;
	}
	while ((entry = readdir(directory)) != NULL) {
		/* Selects the matching value. */
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		fs_length = strlen(filesystem_name) + strlen(entry->d_name) + 2;
		stored_length = strlen(stored_name) + strlen(entry->d_name) + 2;
		child_fs = malloc(fs_length);
		child_stored = malloc(stored_length);

		/* Handles the child fs availability. */
		if (child_fs == NULL || child_stored == NULL) {
			free(child_fs);
			free(child_stored);
			exit_status = 1;
			break;
		}
		(void)snprintf(child_fs, fs_length, "%s/%s", filesystem_name,
			       entry->d_name);
		(void)snprintf(child_stored, stored_length, "%s/%s",
			       stored_name, entry->d_name);
		(void)write_tree(child_fs, child_stored);
		free(child_fs);
		free(child_stored);
	}

	/* Handles a failed closedir operation. */
	if (closedir(directory) != 0)
		warn_path("close directory", filesystem_name);

	/* Reports successful completion. */
	return 0;
}

/* Supports the warn path operation. */
static void
warn_path(
	const char *operation,
	const char *path)
{
	fprintf(stderr, "pax: %s %s: %s\n", operation, path, strerror(errno));
	exit_status = 1;
}

/* Supports the write member operation. */
static int
write_member(
	const char *filesystem_name,
	const char *stored_name,
	const struct stat *status)
{
	ssize_t count;
	struct tar_header header;
	char linkname[PATH_MAX + 1];
	const char *hardlink;
	char type;
	off_t size;
	FILE *input;

	hardlink = NULL;
	size = 0;
	input = NULL;

	/* Handles a failed S ISREG operation. */
	if (S_ISREG(status->st_mode)) {
		/* Checks the operation status. */
		if (status->st_nlink > 1)
			hardlink = known_link(status);
		type = hardlink == NULL ? '0' : '1';
		size = hardlink == NULL ? status->st_size : 0;
	} else if (S_ISDIR(status->st_mode))
		type = '5';
	else if (S_ISLNK(status->st_mode)) {
		count = readlink(filesystem_name, linkname, PATH_MAX);

		/* Checks the remaining item count. */
		if (count < 0) {
			warn_path("readlink", filesystem_name);

			/* Reports operation failure. */
			return -1;
		}
		linkname[count] = '\0';
		type = '2';
	} else if (S_ISFIFO(status->st_mode)) {
		type = '6';
	} else {
		fprintf(stderr, "pax: unsupported file type: %s\n",
			filesystem_name);
		exit_status = 1;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed fill header operation. */
	if (fill_header(&header, stored_name, status, type,
			type == '1'   ? hardlink
			: type == '2' ? linkname
				      : NULL,
			size) != 0) {
		warn_path("archive name", stored_name);

		/* Reports operation failure. */
		return -1;
	}

	/* Handles the type condition. */
	if (type == '0') {
		input = fopen(filesystem_name, "rb");

		/* Handles the input availability. */
		if (input == NULL) {
			warn_path("open", filesystem_name);

			/* Reports operation failure. */
			return -1;
		}
	}

	/* Handles a failed write all operation. */
	if (write_all(archive_file, &header, sizeof(header)) != 0 ||
	    (input != NULL &&
	     copy_stream(input, archive_file, (uint64_t)size) != 0) ||
	    pad_output((uint64_t)size) != 0) {
		warn_path("write archive member", stored_name);

		/* Handles the input availability. */
		if (input != NULL)
			fclose(input);

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed fclose operation. */
	if (input != NULL && fclose(input) != 0)
		warn_path("close", filesystem_name);

	/* Handles a failed S ISREG operation. */
	if (hardlink == NULL && S_ISREG(status->st_mode) &&
	    status->st_nlink > 1)
		(void)remember_link(status, stored_name);

	/* Handles the verbose condition. */
	if (verbose)
		fprintf(stderr, "%s\n", stored_name);

	/* Reports successful completion. */
	return 0;
}

/* Supports the known link operation. */
static const char *
known_link(
	const struct stat *status)
{
	struct hard_link *entry;

	/* Process each linked entry. */
	for (entry = links; entry != NULL; entry = entry->next) {
		/* Handles the entry condition. */
		if (entry->device == status->st_dev &&
		    entry->inode == status->st_ino)

			/* Returns the computed result. */
			return entry->name;
	}

	/* Reports that no result is available. */
	return NULL;
}

/* Supports the fill header operation. */
static int
fill_header(
	struct tar_header *header,
	const char *name,
	const struct stat *status,
	char type,
	const char *linkname,
	off_t size)
{
	unsigned checksum;

	memset(header, 0, sizeof(*header));

	/* Handles a failed split header name operation. */
	if (split_header_name(header, name) != 0 ||
	    format_octal(header->mode, sizeof(header->mode),
			 status->st_mode & 07777) ||
	    format_octal(header->uid, sizeof(header->uid), status->st_uid) ||
	    format_octal(header->gid, sizeof(header->gid), status->st_gid) ||
	    format_octal(header->size, sizeof(header->size), (uint64_t)size) ||
	    format_octal(header->mtime, sizeof(header->mtime),
			 (uint64_t)status->st_mtime))

		/* Reports operation failure. */
		return -1;
	header->type = type;

	/* Handles the linkname availability. */
	if (linkname != NULL) {
		/* Handles a failed strlen operation. */
		if (strlen(linkname) > sizeof(header->linkname)) {
			errno = ENAMETOOLONG;

			/* Reports operation failure. */
			return -1;
		}
		memcpy(header->linkname, linkname, strlen(linkname));
	}
	memcpy(header->magic, "ustar", 5);
	memcpy(header->version, "00", 2);
	checksum = header_checksum(header);
	(void)snprintf(header->checksum, sizeof(header->checksum), "%06o",
		       checksum);
	header->checksum[6] = '\0';
	header->checksum[7] = ' ';

	/* Reports successful completion. */
	return 0;
}

/* Supports the split header name operation. */
static int
split_header_name(
	struct tar_header *header,
	const char *name)
{
	size_t length;
	const char *slash;

	length = strlen(name);

	/* Checks the current data length. */
	if (length <= sizeof(header->name)) {
		memcpy(header->name, name, length);

		/* Reports successful completion. */
		return 0;
	}

	/* Process each remaining element. */
	for (slash = name + length; slash > name; slash--) {
		/* Handles the slash condition. */
		if (slash[-1] != '/')
			continue;

		/* Handles the slash condition. */
		if ((size_t)(slash - name - 1) <= sizeof(header->prefix) &&
		    length - (size_t)(slash - name) <= sizeof(header->name)) {
			memcpy(header->prefix, name,
			       (size_t)(slash - name - 1));
			memcpy(header->name, slash,
			       length - (size_t)(slash - name));

			/* Reports successful completion. */
			return 0;
		}
	}
	errno = ENAMETOOLONG;

	/* Reports operation failure. */
	return -1;
}

/* Supports the format octal operation. */
static int
format_octal(
	char *field,
	size_t length,
	uint64_t value)
{
	char temporary[32];
	int count;

	count = snprintf(temporary, sizeof(temporary), "%0*llo",
			     (int)length - 1, (unsigned long long)value);

	/* Checks the remaining item count. */
	if (count < 0 || (size_t)count >= length)
		return -1;
	memset(field, 0, length);
	memcpy(field, temporary, (size_t)count);

	/* Reports successful completion. */
	return 0;
}

/* Supports the header checksum operation. */
static unsigned
header_checksum(
	const struct tar_header *header)
{
	const unsigned char *bytes;
	unsigned sum;
	size_t index;

	/* Process each remaining element. */
	bytes = (const unsigned char *)header;
	sum = 0;
	for (index = 0; index < sizeof(*header); index++) {
		/* Handles a failed offsetof operation. */
		if (index >= offsetof(struct tar_header, checksum) &&
		    index < offsetof(struct tar_header, checksum) + 8)
			sum += ' ';
		else
			sum += bytes[index];
	}

	/* Returns the computed result. */
	return sum;
}

/* Supports the write all operation. */
static int
write_all(
	FILE *file,
	const void *buffer,
	size_t length)
{
	size_t count;
	const unsigned char *cursor;

	/* Process each remaining element. */
	cursor = buffer;
	while (length != 0) {
		count = fwrite(cursor, 1, length, file);

		/* Checks the remaining item count. */
		if (count == 0)
			return -1;
		cursor += count;
		length -= count;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the copy stream operation. */
static int
copy_stream(
	FILE *input,
	FILE *output,
	uint64_t length)
{
	size_t wanted;
	unsigned char buffer[16384];

	/* Process each remaining element. */
	while (length != 0) {
		wanted = length < sizeof(buffer) ? (size_t)length : sizeof(buffer);

		/* Handles a failed read all operation. */
		if (read_all(input, buffer, wanted) != 1 ||
		    write_all(output, buffer, wanted) != 0)

			/* Reports operation failure. */
			return -1;
		length -= wanted;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the read all operation. */
static int
read_all(
	FILE *file,
	void *buffer,
	size_t length)
{
	int function_result;
	size_t count;
	unsigned char *cursor;

	/* Process each remaining element. */
	cursor = buffer;
	while (length != 0) {
		count = fread(cursor, 1, length, file);

		/* Checks the remaining item count. */
		if (count == 0) {
			/* Computes the function result. */
			function_result = feof(file) ? 0 : -1;

			/* Returns the computed result. */
			return function_result;
		}
		cursor += count;
		length -= count;
	}

	/* Reports operation failure. */
	return 1;
}

/* Supports the pad output operation. */
static int
pad_output(
	uint64_t length)
{
	int function_result;
	static const unsigned char zeros[BLOCK_SIZE];
	size_t padding;

	padding = (size_t)((BLOCK_SIZE - length % BLOCK_SIZE) % BLOCK_SIZE);

	/* Computes the function result. */
	function_result = padding == 0 ? 0 : write_all(archive_file, zeros, padding);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the remember link operation. */
static int
remember_link(
	const struct stat *status,
	const char *name)
{
	struct hard_link *entry;

	entry = malloc(sizeof(*entry));

	/* Handles the entry availability. */
	if (entry == NULL)
		return -1;
	entry->name = strdup(name);

	/* Handles the name availability. */
	if (entry->name == NULL) {
		free(entry);

		/* Reports operation failure. */
		return -1;
	}
	entry->device = status->st_dev;
	entry->inode = status->st_ino;
	entry->next = links;
	links = entry;

	/* Reports successful completion. */
	return 0;
}

/* Supports the finish archive operation. */
static int
finish_archive(
	void)
{
	static const unsigned char zeros[BLOCK_SIZE * 2];

	/* Handles a failed write all operation. */
	if (write_all(archive_file, zeros, sizeof(zeros)) != 0 ||
	    fflush(archive_file) != 0) {
		fprintf(stderr, "pax: could not finish archive: %s\n",
			strerror(errno));

		/* Reports operation failure. */
		return -1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the read archive operation. */
static int
read_archive(
	int extract,
	char **patterns,
	int pattern_count)
{
	char linkname[sizeof(((struct tar_header *)0)->linkname) + 1];
	struct tar_header *header;
	uint64_t size, mode, mtime, stored_checksum;
	char *raw_name;
	char *name;
	int selected;
	int result;
	unsigned char block[BLOCK_SIZE];
	int saw_zero;

	/* Continue until the operation reaches a terminal state. */
	saw_zero = 0;
	for (;;) {
		header = (struct tar_header *)block;
		result = read_all(archive_file, block, sizeof(block));

		/* Checks the operation result. */
		if (result == 0)
			return saw_zero ? 0 : -1;

		/* Checks the operation result. */
		if (result < 0)
			return -1;

		/* Handles the zero block condition. */
		if (zero_block(block)) {
			/* Handles the saw zero condition. */
			if (saw_zero)
				return 0;
			saw_zero = 1;
			continue;
		}
		saw_zero = 0;

		/* Handles a failed parse octal operation. */
		if (parse_octal(header->checksum, sizeof(header->checksum),
				&stored_checksum) != 0 ||
		    stored_checksum != header_checksum(header) ||
		    parse_octal(header->size, sizeof(header->size), &size) !=
			0 ||
		    parse_octal(header->mode, sizeof(header->mode), &mode) !=
			0 ||
		    parse_octal(header->mtime, sizeof(header->mtime), &mtime) !=
			0) {
			fprintf(stderr, "pax: corrupt archive header\n");

			/* Reports operation failure. */
			return -1;
		}
		raw_name = archive_name(header);
		name = raw_name == NULL ? NULL : transform_name(raw_name);
		free(raw_name);

		/* Handles the name availability. */
		if (name == NULL)
			return -1;
		selected = matches_patterns(name, patterns, pattern_count);

		/* Handles the extract condition. */
		if (!extract) {
			/* Handles the selected condition. */
			if (selected)
				printf("%s\n", name);
			free(name);

			/* Handles a failed skip payload operation. */
			if (skip_payload(size) != 0)
				return -1;
			continue;
		}

		/* Handles the selected condition. */
		if (!selected) {
			free(name);

			/* Handles a failed skip payload operation. */
			if (skip_payload(size) != 0)
				return -1;
			continue;
		}

		/* Handles a failed safe path operation. */
		if (!safe_path(name)) {
			fprintf(stderr,
				"pax: refusing unsafe archive path: %s\n",
				name);
			exit_status = 1;
			free(name);

			/* Handles a failed skip payload operation. */
			if (skip_payload(size) != 0)
				return -1;
			continue;
		}

		/* Handles the verbose condition. */
		if (verbose)
			fprintf(stderr, "%s\n", name);

		/* Handles the header condition. */
		if (header->type == '0' || header->type == '\0') {
			/* Handles a failed extract regular operation. */
			if (extract_regular(name, size, (mode_t)mode,
					    (time_t)mtime) != 0) {
				warn_path("extract", name);
				free(name);

				/* Reports operation failure. */
				return -1;
			}

			/* Handles a failed consume padding operation. */
			if (consume_padding(size) != 0) {
				free(name);

				/* Reports operation failure. */
				return -1;
			}
		} else {
			memcpy(linkname, header->linkname,
			       sizeof(header->linkname));
			linkname[sizeof(header->linkname)] = '\0';

			/* Handles a failed skip payload operation. */
			if (skip_payload(size) != 0) {
				free(name);

				/* Reports operation failure. */
				return -1;
			}

			/* Handles the header condition. */
			if (header->type == '5') {
				/* Handles the reported system error. */
				if (make_parents(name) != 0 ||
				    (mkdir(name, (mode_t)mode) != 0 &&
				     errno != EEXIST))
					warn_path("mkdir", name);
				else
					apply_metadata(name, (mode_t)mode,
						       (time_t)mtime, 0);
			} else if (header->type == '2') {
				/* Handles a failed safe path operation. */
				if (!safe_path(linkname) ||
				    make_parents(name) != 0 ||
				    remove_existing(name, 0) != 0 ||
				    symlink(linkname, name) != 0)
					warn_path("symlink", name);
			} else if (header->type == '1') {
				/* Handles a failed safe path operation. */
				if (!safe_path(linkname) ||
				    make_parents(name) != 0 ||
				    remove_existing(name, 0) != 0 ||
				    link(linkname, name) != 0)
					warn_path("hard link", name);
			} else if (header->type == '6') {
				/* Handles a failed make parents operation. */
				if (make_parents(name) != 0 ||
				    remove_existing(name, 0) != 0 ||
				    mkfifo(name, (mode_t)mode) != 0)
					warn_path("fifo", name);
			} else {
				fprintf(stderr,
					"pax: unsupported archive member type "
					"%c: %s\n",
					header->type, name);
				exit_status = 1;
			}
		}
		free(name);
	}
}

/* Supports the zero block operation. */
static int
zero_block(
	const unsigned char *block)
{
	size_t index;

	/* Process each remaining element. */
	for (index = 0; index < BLOCK_SIZE; index++) {
		/* Handles the block condition. */
		if (block[index] != 0)
			return 0;
	}

	/* Reports operation failure. */
	return 1;
}

/* Supports the parse octal operation. */
static int
parse_octal(
	const char *field,
	size_t length,
	uint64_t *value)
{
	unsigned digit;
	size_t index;
	uint64_t result;

	/* Process each remaining element. */
	index = 0;
	result = 0;
	while (index < length && (field[index] == ' ' || field[index] == '\0'))
		index++;

	/* Process each remaining element. */
	for (; index < length && field[index] != '\0' && field[index] != ' ';
	     index++) {
		/* Handles the field condition. */
		if (field[index] < '0' || field[index] > '7')
			return -1;
		digit = (unsigned)(field[index] - '0');

		/* Checks the operation result. */
		if (result > (UINT64_MAX - digit) / 8)
			return -1;
		result = result * 8 + digit;
	}
	*value = result;
	/* Reports successful completion. */
	return 0;
}

/* Supports the archive name operation. */
static char *
archive_name(
	const struct tar_header *header)
{
	size_t prefix_length;
	size_t name_length;
	char *name;

	prefix_length = strnlen(header->prefix, sizeof(header->prefix));
	name_length = strnlen(header->name, sizeof(header->name));
	name = malloc(prefix_length + name_length + 2);

	/* Handles the name availability. */
	if (name == NULL)
		return NULL;

	/* Handles the prefix length condition. */
	if (prefix_length != 0) {
		memcpy(name, header->prefix, prefix_length);
		name[prefix_length++] = '/';
	}
	memcpy(name + prefix_length, header->name, name_length);
	name[prefix_length + name_length] = '\0';

	/* Returns the computed result. */
	return name;
}

/* Supports the transform name operation. */
static char *
transform_name(
	const char *name)
{
	int matched;
	char *next;
	struct substitution *rule;
	char *current;

	current = strdup(name);

	/* Handles the current availability. */
	if (current == NULL)
		return NULL;

	/* Process each linked entry. */
	for (rule = substitutions; rule != NULL; rule = rule->next) {
		next = replace_once(current, rule, &matched);
		free(current);

		/* Handles the next availability. */
		if (next == NULL)
			return NULL;
		current = next;

		/* Handles the matched condition. */
		if (matched)
			break;
	}

	/* Returns the computed result. */
	return current;
}

/* Supports the replace once operation. */
static char *
replace_once(
	const char *input,
	const struct substitution *rule,
	int *matched)
{
	size_t group_length;
	int group;
	regmatch_t *whole;
	const char *replacement;
	size_t prefix;
	regmatch_t matches[10];
	size_t capacity;
	size_t length;
	char *output;
	const char *cursor;

	capacity = strlen(input) + strlen(rule->replacement) + 32;
	length = 0;
	output = malloc(capacity);
	cursor = input;

	/* Handles the output availability. */
	if (output == NULL)
		return NULL;
	*matched = 0;
	do {
		whole = &matches[0];

		/* Handles a failed regexec operation. */
		if (regexec(&rule->expression, cursor, 10, matches, 0) != 0)
			break;
		*matched = 1;
		prefix = (size_t)whole->rm_so;
#define ENSURE(extra)                                                          \
	do {                                                                   \
		if ((extra) > SIZE_MAX - length - 1) {                         \
			free(output);                                          \
			return NULL;                                           \
		}                                                              \
		if (length + (extra) + 1 > capacity) {                         \
			size_t new_capacity = (length + (extra) + 1) * 2;      \
			char *new_output = realloc(output, new_capacity);      \
			if (new_output == NULL) {                              \
				free(output);                                  \
				return NULL;                                   \
			}                                                      \
			output = new_output;                                   \
			capacity = new_capacity;                               \
		}                                                              \
	} while (0)
		ENSURE(prefix);
		memcpy(output + length, cursor, prefix);
		length += prefix;

		/* Process each element required by the operation. */
		for (replacement = rule->replacement; *replacement != '\0';
		     replacement++) {
			group = -1;

			/* Handles the replacement condition. */
			if (*replacement == '&')
				group = 0;
			else if (*replacement == '\\' &&
				 replacement[1] >= '0' && replacement[1] <= '9')
				group = *++replacement - '0';
			else if (*replacement == '\\' && replacement[1] != '\0')
				replacement++;

			/* Handles the group condition. */
			if (group >= 0 && matches[group].rm_so >= 0) {
				group_length = (size_t)(matches[group].rm_eo -
			     matches[group].rm_so);
				ENSURE(group_length);
				memcpy(output + length,
				       cursor + matches[group].rm_so,
				       group_length);
				length += group_length;
			} else if (group < 0) {
				ENSURE(1);
				output[length++] = *replacement;
			}
		}
		cursor += whole->rm_eo;

		/* Handles the rule condition. */
		if (!rule->global || whole->rm_so == whole->rm_eo)
			break;
	} while (*cursor != '\0');
	ENSURE(strlen(cursor));
	strcpy(output + length, cursor);
#undef ENSURE

	/* Returns the computed result. */
	return output;
}

/* Supports the matches patterns operation. */
static int
matches_patterns(
	const char *name,
	char **patterns,
	int pattern_count)
{
	int index;

	/* Handles the pattern count condition. */
	if (pattern_count == 0)
		return 1;

	/* Process each remaining element. */
	for (index = 0; index < pattern_count; index++) {
		/* Handles a failed fnmatch operation. */
		if (fnmatch(patterns[index], name, 0) == 0)
			return 1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the skip payload operation. */
static int
skip_payload(
	uint64_t length)
{
	size_t amount;
	unsigned char buffer[4096];
	uint64_t total;

	/* Continue while the operation condition remains true. */
	total = length + (BLOCK_SIZE - length % BLOCK_SIZE) % BLOCK_SIZE;
	while (total != 0) {
		amount = total < sizeof(buffer) ? (size_t)total : sizeof(buffer);

		/* Handles a failed read all operation. */
		if (read_all(archive_file, buffer, amount) != 1)
			return -1;
		total -= amount;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the safe path operation. */
static int
safe_path(
	const char *path)
{
	const char *slash;
	const char *part;
	size_t length;

	part = path;

	/* Handles the path condition. */
	if (path[0] == '/' || path[0] == '\0')
		return 0;

	/* Continue while the operation condition remains true. */
	while (*part != '\0') {
		slash = strchr(part, '/');
		length = slash == NULL ? strlen(part)
				       : (size_t)(slash - part);

		/* Checks the current data length. */
		if (length == 2 && part[0] == '.' && part[1] == '.')
			return 0;
		part = slash == NULL ? part + length : slash + 1;
	}

	/* Reports operation failure. */
	return 1;
}

/* Supports the extract regular operation. */
static int
extract_regular(
	const char *path,
	uint64_t size,
	mode_t mode,
	time_t mtime)
{
	char *template;
	size_t length;
	int descriptor;
	FILE *output;
	int failed;

	length = strlen(path) + 16;
	failed = 0;

	/* Handles a failed make parents operation. */
	if (make_parents(path) != 0)
		return -1;
	template = malloc(length);

	/* Handles the template availability. */
	if (template == NULL)
		return -1;
	(void)snprintf(template, length, "%s.pax.XXXXXX", path);
	descriptor = mkstemp(template);

	/* Checks the file descriptor. */
	if (descriptor < 0) {
		free(template);

		/* Reports operation failure. */
		return -1;
	}
	output = fdopen(descriptor, "wb");

	/* Handles the output availability. */
	if (output == NULL) {
		close(descriptor);
		unlink(template);
		free(template);

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed copy stream operation. */
	if (copy_stream(archive_file, output, size) != 0 || fclose(output) != 0)
		failed = 1;

	/* Handles an operation failure. */
	if (!failed && remove_existing(path, 0) != 0)
		failed = 1;

	/* Handles an operation failure. */
	if (!failed && rename(template, path) != 0)
		failed = 1;

	/* Handles an operation failure. */
	if (failed)
		unlink(template);
	else
		apply_metadata(path, mode, mtime, 0);
	free(template);

	/* Returns the computed result. */
	return failed ? -1 : 0;
}

/* Supports the make parents operation. */
static int
make_parents(
	const char *path)
{
	char *copy;
	char *cursor;
	struct stat status;

	copy = strdup(path);

	/* Handles the copy availability. */
	if (copy == NULL)
		return -1;

	/* Process each element required by the operation. */
	for (cursor = copy + 1; *cursor != '\0'; cursor++) {
		/* Checks the current cursor position. */
		if (*cursor != '/')
			continue;
		*cursor = '\0';
		/* Handles a failed lstat operation. */
		if (lstat(copy, &status) == 0) {
			/* Handles a failed S ISDIR operation. */
			if (!S_ISDIR(status.st_mode) ||
			    S_ISLNK(status.st_mode)) {
				errno = ELOOP;
				free(copy);

				/* Reports operation failure. */
				return -1;
			}
		} else if (errno == ENOENT) {
			/* Handles a failed mkdir operation. */
			if (mkdir(copy, 0777) != 0) {
				free(copy);

				/* Reports operation failure. */
				return -1;
			}
		} else {
			free(copy);

			/* Reports operation failure. */
			return -1;
		}
		*cursor = '/';
	}
	free(copy);

	/* Reports successful completion. */
	return 0;
}

/* Supports the remove existing operation. */
static int
remove_existing(
	const char *path,
	int directory)
{
	int function_result;
	struct stat status;

	/* Handles a failed lstat operation. */
	if (lstat(path, &status) != 0)
		return errno == ENOENT ? 0 : -1;

	/* Handles the directory condition. */
	if (directory && S_ISDIR(status.st_mode))
		return 0;

	/* Computes the function result. */
	function_result = S_ISDIR(status.st_mode) ? rmdir(path) : unlink(path);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the apply metadata operation. */
static void
apply_metadata(
	const char *path,
	mode_t mode,
	time_t modification,
	int symlink)
{
	struct timespec times[2];

	/* Handles a failed chmod operation. */
	if (!symlink && preserve_mode && chmod(path, mode & 07777) != 0)
		warn_path("chmod", path);

	/* Handles the preserve time condition. */
	if (preserve_time) {
		times[0].tv_sec = modification;
		times[0].tv_nsec = 0;
		times[1] = times[0];

		/* Handles a failed utimensat operation. */
		if (utimensat(AT_FDCWD, path, times,
			      symlink ? AT_SYMLINK_NOFOLLOW : 0) != 0)
			warn_path("set time", path);
	}
}

/* Supports the consume padding operation. */
static int
consume_padding(
	uint64_t size)
{
	int function_result;
	unsigned char padding[BLOCK_SIZE];
	size_t count;

	count = (size_t)((BLOCK_SIZE - size % BLOCK_SIZE) % BLOCK_SIZE);

	/* Computes the function result. */
	function_result = count == 0 || read_all(archive_file, padding, count) == 1 ? 0
									 : -1;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the position for append operation. */
static int
position_for_append(
	FILE *file)
{
	int function_result;
	struct tar_header *header;
	uint64_t size;
	off_t position;
	unsigned char block[BLOCK_SIZE];

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		header = (struct tar_header *)block;
		position = ftello(file);

		/* Handles a failed read all operation. */
		if (position < 0 || read_all(file, block, sizeof(block)) != 1)
			return -1;

		/* Handles the zero block condition. */
		if (zero_block(block)) {
			/* Obtains the fseeko result. */
			function_result = fseeko(file, position, SEEK_SET);

			/* Returns the computed result. */
			return function_result;
		}

		/* Handles a failed parse octal operation. */
		if (parse_octal(header->size, sizeof(header->size), &size) !=
			0 ||
		    size > (uint64_t)INT64_MAX)

			/* Reports operation failure. */
			return -1;
		size += (BLOCK_SIZE - size % BLOCK_SIZE) % BLOCK_SIZE;

		/* Handles a failed fseeko operation. */
		if (fseeko(file, (off_t)size, SEEK_CUR) != 0)
			return -1;
	}
}
