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

/* The largest extension header body this reader accepts. */
#define EXTENSION_MAX 1048576U

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

/*
 * What the extension headers before a member say about it.
 *
 * A pax extended header (type x) or a GNU long name header (type L or K)
 * describes the ustar header that follows it.  The fields it overrides
 * collect here and are taken by that next member, which empties it again.
 */
struct extension {
	char *path;
	char *linkpath;
	int have_size;
	uint64_t size;
	int have_mtime;
	uint64_t mtime;
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
static int extract_member(char type, const char *name, const char *linkname, uint64_t size, uint64_t mode, uint64_t mtime);
static void extract_directory(const char *name, mode_t mode, time_t mtime);
static void extract_link(const char *name, const char *linkname, int symbolic);
static void extract_fifo(const char *name, mode_t mode);
static int read_extension(char type, uint64_t size, struct extension *extension);
static int parse_extension_records(const char *records, size_t length, struct extension *extension);
static int apply_extension_record(const char *key, size_t key_length, const char *value, size_t value_length, struct extension *extension);
static int key_is(const char *key, size_t key_length, const char *word);
static int parse_decimal(const char *text, size_t length, uint64_t *value);
static int take_extension(struct extension *extension, const struct tar_header *header, char **raw_name, char **linkname, uint64_t *size, uint64_t *mtime);
static char *header_linkname(const struct tar_header *header);
static void clear_extension(struct extension *extension);
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
	struct extension extension;
	struct tar_header *header;
	uint64_t size, mode, mtime, stored_checksum;
	char *raw_name;
	char *name;
	char *linkname;
	int selected;
	int skip;
	int safe;
	int result;
	int error;
	unsigned char block[BLOCK_SIZE];
	int saw_zero;

	/* No extension header has been read yet. */
	memset(&extension, 0, sizeof(extension));

	/* Reads one header block per member until the end of the archive. */
	saw_zero = 0;
	for (;;) {
		header = (struct tar_header *)block;
		result = read_all(archive_file, block, sizeof(block));

		/* An archive that ends without the zero blocks is short. */
		if (result == 0) {
			clear_extension(&extension);

			/* Reports the end of the archive, or that it was cut. */
			return saw_zero ? 0 : -1;
		}

		/* Reports a read error. */
		if (result < 0) {
			clear_extension(&extension);
			return -1;
		}

		/* Two zero blocks in a row end the archive. */
		if (zero_block(block)) {
			/* Reports the end of the archive after the second one. */
			if (saw_zero) {
				clear_extension(&extension);
				return 0;
			}
			saw_zero = 1;
			continue;
		}
		saw_zero = 0;

		/* Refuses a header whose checksum or numeric fields are bad. */
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
			clear_extension(&extension);

			/* Reports the corrupt header. */
			return -1;
		}

		/* An extension header describes the member that follows it. */
		if (header->type == 'x' || header->type == 'g' ||
		    header->type == 'L' || header->type == 'K') {
			error = read_extension(header->type, size, &extension);
			if (error != 0) {
				clear_extension(&extension);
				return -1;
			}

			/* The member it describes is the next header. */
			continue;
		}

		/* Resolves the member's name, link target, size and time. */
		error = take_extension(&extension, header, &raw_name, &linkname,
				       &size, &mtime);
		if (error != 0)
			return -1;

		/* Applies the -s substitutions to the name. */
		name = transform_name(raw_name);
		free(raw_name);
		if (name == NULL) {
			free(linkname);
			return -1;
		}
		selected = matches_patterns(name, patterns, pattern_count);

		/* Decides whether the member is extracted or only passed over. */
		skip = 0;
		if (!extract) {
			/* Lists a selected member instead of extracting it. */
			if (selected)
				printf("%s\n", name);
			skip = 1;
		} else if (!selected) {
			skip = 1;
		} else {
			safe = safe_path(name);

			/* Refuses a name that would escape the destination. */
			if (!safe) {
				fprintf(stderr,
					"pax: refusing unsafe archive path: %s\n",
					name);
				exit_status = 1;
				skip = 1;
			}
		}

		/* Passes over the body of a member that is not extracted. */
		if (skip) {
			free(name);
			free(linkname);
			error = skip_payload(size);
			if (error != 0)
				return -1;
			continue;
		}

		/* Names the member on -v. */
		if (verbose)
			fprintf(stderr, "%s\n", name);

		/* Creates the member in the file system. */
		error = extract_member(header->type, name, linkname, size, mode,
				       mtime);
		free(name);
		free(linkname);
		if (error != 0)
			return -1;
	}
}

/* Creates one member of the archive from its resolved header fields. */
static int
extract_member(
	char type,
	const char *name,
	const char *linkname,
	uint64_t size,
	uint64_t mode,
	uint64_t mtime)
{
	int error;

	/* A regular file is its body; nothing else has one worth reading. */
	if (type == '0' || type == '\0') {
		error = extract_regular(name, size, (mode_t)mode, (time_t)mtime);
		if (error != 0) {
			warn_path("extract", name);
			return -1;
		}

		/* Skips the padding after the body. */
		error = consume_padding(size);
		if (error != 0)
			return -1;

		/* Succeeded: the file is written. */
		return 0;
	}

	/* Passes over a body that a non-regular member should not have. */
	error = skip_payload(size);
	if (error != 0)
		return -1;

	/* Creates the directory, link or fifo the header describes. */
	if (type == '5') {
		extract_directory(name, (mode_t)mode, (time_t)mtime);
	} else if (type == '2') {
		extract_link(name, linkname, 1);
	} else if (type == '1') {
		extract_link(name, linkname, 0);
	} else if (type == '6') {
		extract_fifo(name, (mode_t)mode);
	} else {
		fprintf(stderr, "pax: unsupported archive member type %c: %s\n",
			type, name);
		exit_status = 1;
	}

	/* Succeeded: a member that could not be made was reported, not fatal. */
	return 0;
}

/* Creates a directory member, reusing one that already exists. */
static void
extract_directory(
	const char *name,
	mode_t mode,
	time_t mtime)
{
	int error;

	/* The parents come first. */
	error = make_parents(name);
	if (error != 0) {
		warn_path("mkdir", name);
		return;
	}

	/* Makes the directory itself; one that already exists is kept. */
	error = mkdir(name, mode);
	if (error != 0 && errno != EEXIST) {
		warn_path("mkdir", name);
		return;
	}

	/* Gives the directory the archive's mode and time. */
	apply_metadata(name, mode, mtime, 0);
}

/* Creates a symbolic or hard link member in place of whatever is there. */
static void
extract_link(
	const char *name,
	const char *linkname,
	int symbolic)
{
	const char *operation;
	int safe;
	int error;

	/* Names the kind of link in any warning. */
	if (symbolic)
		operation = "symlink";
	else
		operation = "hard link";

	/* Refuses a target that would escape the destination. */
	safe = safe_path(linkname);
	if (!safe) {
		warn_path(operation, name);
		return;
	}

	/* Makes room for the link. */
	error = make_parents(name);
	if (error == 0)
		error = remove_existing(name, 0);
	if (error != 0) {
		warn_path(operation, name);
		return;
	}

	/* Makes the link of the requested kind. */
	if (symbolic)
		error = symlink(linkname, name);
	else
		error = link(linkname, name);
	if (error != 0)
		warn_path(operation, name);
}

/* Creates a fifo member in place of whatever is there. */
static void
extract_fifo(
	const char *name,
	mode_t mode)
{
	int error;

	/* Makes room for the fifo, then makes it. */
	error = make_parents(name);
	if (error == 0)
		error = remove_existing(name, 0);
	if (error == 0)
		error = mkfifo(name, mode);
	if (error != 0)
		warn_path("fifo", name);
}

/* Reads the body of an extension header into the pending extension. */
static int
read_extension(
	char type,
	uint64_t size,
	struct extension *extension)
{
	char *body;
	size_t length;
	int result;
	int error;

	/* Refuses a body too large to be a set of records or a name. */
	if (size > EXTENSION_MAX) {
		fprintf(stderr, "pax: extended header too large\n");
		return -1;
	}

	/* Reads the whole body with a terminator after it. */
	length = (size_t)size;
	body = malloc(length + 1);
	if (body == NULL)
		return -1;
	result = read_all(archive_file, body, length);
	if (result != 1) {
		free(body);
		return -1;
	}

	/* The terminator lets a name body be used as a string. */
	body[length] = '\0';

	/* Skips the padding after the body. */
	error = consume_padding(size);
	if (error != 0) {
		free(body);
		return -1;
	}

	/* A GNU long name (L) or long link name (K) is the body itself. */
	if (type == 'L') {
		free(extension->path);
		extension->path = body;
		return 0;
	}

	/* The later of two long link names wins, as for the path. */
	if (type == 'K') {
		free(extension->linkpath);
		extension->linkpath = body;
		return 0;
	}

	/*
	 * A global header (g) sets defaults for the rest of the archive.  GNU
	 * tar only puts a comment there, so its records are not applied.
	 */
	if (type == 'g') {
		free(body);
		return 0;
	}

	/* A pax extended header (x) is a list of records for the next member. */
	error = parse_extension_records(body, length, extension);
	free(body);
	if (error != 0) {
		fprintf(stderr, "pax: corrupt extended header\n");
		return -1;
	}

	/* Succeeded: the records are pending for the next member. */
	return 0;
}

/* Applies every "length key=value" record of a pax extended header. */
static int
parse_extension_records(
	const char *records,
	size_t length,
	struct extension *extension)
{
	size_t offset;
	size_t cursor;
	size_t record_length;
	size_t end;
	const char *key;
	size_t key_length;
	const char *value;
	size_t value_length;
	int error;

	/* Walks the records; each one starts with its own length in decimal. */
	offset = 0;
	while (offset < length) {
		record_length = 0;
		cursor = offset;
		while (cursor < length && records[cursor] >= '0' &&
		       records[cursor] <= '9') {
			/* Refuses a length that would not fit. */
			if (record_length > (SIZE_MAX - 9) / 10)
				return -1;
			record_length =
				record_length * 10 + (size_t)(records[cursor] - '0');
			cursor++;
		}

		/* The length is followed by one space. */
		if (cursor == offset || cursor >= length || records[cursor] != ' ')
			return -1;
		cursor++;

		/* The length counts the digits, the space and the newline. */
		if (record_length < cursor - offset + 1 ||
		    record_length > length - offset)
			return -1;
		end = offset + record_length - 1;
		if (records[end] != '\n')
			return -1;

		/* The key runs up to the equals sign, the value to the newline. */
		key = records + cursor;
		while (cursor < end && records[cursor] != '=')
			cursor++;
		if (cursor == end)
			return -1;
		key_length = (size_t)(records + cursor - key);
		value = records + cursor + 1;
		value_length = end - (cursor + 1);

		/* Keeps the record when it is one this reader understands. */
		error = apply_extension_record(key, key_length, value,
					       value_length, extension);
		if (error != 0)
			return -1;
		offset += record_length;
	}

	/* Succeeded: every record was well formed. */
	return 0;
}

/* Stores one extended header record that overrides a ustar field. */
static int
apply_extension_record(
	const char *key,
	size_t key_length,
	const char *value,
	size_t value_length,
	struct extension *extension)
{
	char *copy;
	int is_path;
	int is_linkpath;
	int is_size;
	int is_mtime;
	int error;

	/* Recognizes the keywords this reader applies. */
	is_path = key_is(key, key_length, "path");
	is_linkpath = key_is(key, key_length, "linkpath");
	is_size = key_is(key, key_length, "size");
	is_mtime = key_is(key, key_length, "mtime");

	/* path and linkpath replace the name and link name of the member. */
	if (is_path || is_linkpath) {
		copy = malloc(value_length + 1);
		if (copy == NULL)
			return -1;
		memcpy(copy, value, value_length);
		copy[value_length] = '\0';

		/* The later of two records for the same field wins. */
		if (is_path) {
			free(extension->path);
			extension->path = copy;
		} else {
			free(extension->linkpath);
			extension->linkpath = copy;
		}

		/* Succeeded: the string waits for the next member. */
		return 0;
	}

	/* size is the true length of a body the octal field cannot hold. */
	if (is_size) {
		error = parse_decimal(value, value_length, &extension->size);
		if (error != 0)
			return -1;
		extension->have_size = 1;

		/* Succeeded: the size waits for the next member. */
		return 0;
	}

	/* mtime may carry a fraction; whole seconds are all this reader keeps. */
	if (is_mtime) {
		error = parse_decimal(value, value_length, &extension->mtime);
		if (error != 0)
			return -1;
		extension->have_mtime = 1;

		/* Succeeded: the time waits for the next member. */
		return 0;
	}

	/* Succeeded: any other keyword (uid, gid, charset, ...) is ignored. */
	return 0;
}

/* Reports whether a record keyword is the given word. */
static int
key_is(
	const char *key,
	size_t key_length,
	const char *word)
{
	size_t word_length;
	int difference;
	int same;

	/* The keyword must match the whole word, not just a prefix of it. */
	word_length = strlen(word);
	same = 0;
	if (key_length == word_length) {
		difference = memcmp(key, word, key_length);
		if (difference == 0)
			same = 1;
	}

	/* Reports the comparison. */
	return same;
}

/* Parses the whole-number part of a decimal record value. */
static int
parse_decimal(
	const char *text,
	size_t length,
	uint64_t *value)
{
	size_t index;
	unsigned digit;
	uint64_t result;

	/* Reads digits up to the end or a fraction point. */
	index = 0;
	result = 0;
	while (index < length && text[index] != '.') {
		/* Refuses a sign or any other non-digit. */
		if (text[index] < '0' || text[index] > '9')
			return -1;
		digit = (unsigned)(text[index] - '0');

		/* Refuses a number that does not fit. */
		if (result > (UINT64_MAX - digit) / 10)
			return -1;
		result = result * 10 + digit;
		index++;
	}

	/* An empty value is not a number. */
	if (index == 0)
		return -1;
	*value = result;

	/* Succeeded: the fraction, if any, is dropped. */
	return 0;
}

/* Takes the member's name, link target, size and time from the pending extension, else the ustar header. */
static int
take_extension(
	struct extension *extension,
	const struct tar_header *header,
	char **raw_name,
	char **linkname,
	uint64_t *size,
	uint64_t *mtime)
{
	/* The extension's path replaces the ustar prefix and name. */
	if (extension->path != NULL) {
		*raw_name = extension->path;
		extension->path = NULL;
	} else {
		*raw_name = archive_name(header);
	}

	/* The extension's link path replaces the ustar link name. */
	if (extension->linkpath != NULL) {
		*linkname = extension->linkpath;
		extension->linkpath = NULL;
	} else {
		*linkname = header_linkname(header);
	}

	/* The extension's size and time replace the octal fields. */
	if (extension->have_size)
		*size = extension->size;
	if (extension->have_mtime)
		*mtime = extension->mtime;
	clear_extension(extension);

	/* Reports an allocation failure with nothing left behind. */
	if (*raw_name == NULL || *linkname == NULL) {
		free(*raw_name);
		free(*linkname);
		return -1;
	}

	/* Succeeded: the caller owns both strings. */
	return 0;
}

/* Copies the ustar link name field as a terminated string. */
static char *
header_linkname(
	const struct tar_header *header)
{
	size_t length;
	char *linkname;

	/* The field is not terminated when the name fills it. */
	length = strnlen(header->linkname, sizeof(header->linkname));
	linkname = malloc(length + 1);
	if (linkname == NULL)
		return NULL;
	memcpy(linkname, header->linkname, length);
	linkname[length] = '\0';

	/* Succeeded: the caller owns the copy. */
	return linkname;
}

/* Frees what a pending extension holds and leaves it empty. */
static void
clear_extension(
	struct extension *extension)
{
	/* Drops the strings and forgets the numeric overrides. */
	free(extension->path);
	free(extension->linkpath);
	memset(extension, 0, sizeof(*extension));
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
