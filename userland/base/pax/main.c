/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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

static void
warn_path(const char *operation, const char *path)
{
	fprintf(stderr, "pax: %s %s: %s\n", operation, path, strerror(errno));
	exit_status = 1;
}

static int
write_all(FILE *file, const void *buffer, size_t length)
{
	const unsigned char *cursor = buffer;
	while (length != 0) {
		size_t count = fwrite(cursor, 1, length, file);
		if (count == 0)
			return -1;
		cursor += count;
		length -= count;
	}
	return 0;
}

static int
read_all(FILE *file, void *buffer, size_t length)
{
	unsigned char *cursor = buffer;
	while (length != 0) {
		size_t count = fread(cursor, 1, length, file);
		if (count == 0)
			return feof(file) ? 0 : -1;
		cursor += count;
		length -= count;
	}
	return 1;
}

static int
parse_octal(const char *field, size_t length, uint64_t *value)
{
	size_t index = 0;
	uint64_t result = 0;
	while (index < length && (field[index] == ' ' || field[index] == '\0'))
		index++;
	for (; index < length && field[index] != '\0' && field[index] != ' ';
	     index++) {
		unsigned digit;
		if (field[index] < '0' || field[index] > '7')
			return -1;
		digit = (unsigned)(field[index] - '0');
		if (result > (UINT64_MAX - digit) / 8)
			return -1;
		result = result * 8 + digit;
	}
	*value = result;
	return 0;
}

static int
format_octal(char *field, size_t length, uint64_t value)
{
	char temporary[32];
	int count = snprintf(temporary, sizeof(temporary), "%0*llo",
			     (int)length - 1, (unsigned long long)value);
	if (count < 0 || (size_t)count >= length)
		return -1;
	memset(field, 0, length);
	memcpy(field, temporary, (size_t)count);
	return 0;
}

static unsigned
header_checksum(const struct tar_header *header)
{
	const unsigned char *bytes = (const unsigned char *)header;
	unsigned sum = 0;
	size_t index;
	for (index = 0; index < sizeof(*header); index++) {
		if (index >= offsetof(struct tar_header, checksum) &&
		    index < offsetof(struct tar_header, checksum) + 8)
			sum += ' ';
		else
			sum += bytes[index];
	}
	return sum;
}

static int
zero_block(const unsigned char *block)
{
	size_t index;
	for (index = 0; index < BLOCK_SIZE; index++)
		if (block[index] != 0)
			return 0;
	return 1;
}

static char *
archive_name(const struct tar_header *header)
{
	size_t prefix_length = strnlen(header->prefix, sizeof(header->prefix));
	size_t name_length = strnlen(header->name, sizeof(header->name));
	char *name = malloc(prefix_length + name_length + 2);
	if (name == NULL)
		return NULL;
	if (prefix_length != 0) {
		memcpy(name, header->prefix, prefix_length);
		name[prefix_length++] = '/';
	}
	memcpy(name + prefix_length, header->name, name_length);
	name[prefix_length + name_length] = '\0';
	return name;
}

static const char *
portable_name(const char *name)
{
	while (name[0] == '/' || (name[0] == '.' && name[1] == '/'))
		name += name[0] == '/' ? 1 : 2;
	return *name == '\0' ? "." : name;
}

static int
safe_path(const char *path)
{
	const char *part = path;
	if (path[0] == '/' || path[0] == '\0')
		return 0;
	while (*part != '\0') {
		const char *slash = strchr(part, '/');
		size_t length =
		    slash == NULL ? strlen(part) : (size_t)(slash - part);
		if (length == 2 && part[0] == '.' && part[1] == '.')
			return 0;
		part = slash == NULL ? part + length : slash + 1;
	}
	return 1;
}

static int
make_parents(const char *path)
{
	char *copy = strdup(path);
	char *cursor;
	struct stat status;
	if (copy == NULL)
		return -1;
	for (cursor = copy + 1; *cursor != '\0'; cursor++) {
		if (*cursor != '/')
			continue;
		*cursor = '\0';
		if (lstat(copy, &status) == 0) {
			if (!S_ISDIR(status.st_mode) ||
			    S_ISLNK(status.st_mode)) {
				errno = ELOOP;
				free(copy);
				return -1;
			}
		} else if (errno == ENOENT) {
			if (mkdir(copy, 0777) != 0) {
				free(copy);
				return -1;
			}
		} else {
			free(copy);
			return -1;
		}
		*cursor = '/';
	}
	free(copy);
	return 0;
}

static int
split_header_name(struct tar_header *header, const char *name)
{
	size_t length = strlen(name);
	const char *slash;
	if (length <= sizeof(header->name)) {
		memcpy(header->name, name, length);
		return 0;
	}
	for (slash = name + length; slash > name; slash--) {
		if (slash[-1] != '/')
			continue;
		if ((size_t)(slash - name - 1) <= sizeof(header->prefix) &&
		    length - (size_t)(slash - name) <= sizeof(header->name)) {
			memcpy(header->prefix, name,
			       (size_t)(slash - name - 1));
			memcpy(header->name, slash,
			       length - (size_t)(slash - name));
			return 0;
		}
	}
	errno = ENAMETOOLONG;
	return -1;
}

static int
fill_header(struct tar_header *header, const char *name,
	    const struct stat *status, char type, const char *linkname,
	    off_t size)
{
	unsigned checksum;
	memset(header, 0, sizeof(*header));
	if (split_header_name(header, name) != 0 ||
	    format_octal(header->mode, sizeof(header->mode),
			 status->st_mode & 07777) ||
	    format_octal(header->uid, sizeof(header->uid), status->st_uid) ||
	    format_octal(header->gid, sizeof(header->gid), status->st_gid) ||
	    format_octal(header->size, sizeof(header->size), (uint64_t)size) ||
	    format_octal(header->mtime, sizeof(header->mtime),
			 (uint64_t)status->st_mtime))
		return -1;
	header->type = type;
	if (linkname != NULL) {
		if (strlen(linkname) > sizeof(header->linkname)) {
			errno = ENAMETOOLONG;
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
	return 0;
}

static int
copy_stream(FILE *input, FILE *output, uint64_t length)
{
	unsigned char buffer[16384];
	while (length != 0) {
		size_t wanted =
		    length < sizeof(buffer) ? (size_t)length : sizeof(buffer);
		if (read_all(input, buffer, wanted) != 1 ||
		    write_all(output, buffer, wanted) != 0)
			return -1;
		length -= wanted;
	}
	return 0;
}

static int
pad_output(uint64_t length)
{
	static const unsigned char zeros[BLOCK_SIZE];
	size_t padding =
	    (size_t)((BLOCK_SIZE - length % BLOCK_SIZE) % BLOCK_SIZE);
	return padding == 0 ? 0 : write_all(archive_file, zeros, padding);
}

static int
skip_payload(uint64_t length)
{
	unsigned char buffer[4096];
	uint64_t total =
	    length + (BLOCK_SIZE - length % BLOCK_SIZE) % BLOCK_SIZE;
	while (total != 0) {
		size_t amount =
		    total < sizeof(buffer) ? (size_t)total : sizeof(buffer);
		if (read_all(archive_file, buffer, amount) != 1)
			return -1;
		total -= amount;
	}
	return 0;
}

static const char *
known_link(const struct stat *status)
{
	struct hard_link *entry;
	for (entry = links; entry != NULL; entry = entry->next)
		if (entry->device == status->st_dev &&
		    entry->inode == status->st_ino)
			return entry->name;
	return NULL;
}

static int
remember_link(const struct stat *status, const char *name)
{
	struct hard_link *entry = malloc(sizeof(*entry));
	if (entry == NULL)
		return -1;
	entry->name = strdup(name);
	if (entry->name == NULL) {
		free(entry);
		return -1;
	}
	entry->device = status->st_dev;
	entry->inode = status->st_ino;
	entry->next = links;
	links = entry;
	return 0;
}

static int
write_member(const char *filesystem_name, const char *stored_name,
	     const struct stat *status)
{
	struct tar_header header;
	char linkname[PATH_MAX + 1];
	const char *hardlink = NULL;
	char type;
	off_t size = 0;
	FILE *input = NULL;

	if (S_ISREG(status->st_mode)) {
		if (status->st_nlink > 1)
			hardlink = known_link(status);
		type = hardlink == NULL ? '0' : '1';
		size = hardlink == NULL ? status->st_size : 0;
	} else if (S_ISDIR(status->st_mode))
		type = '5';
	else if (S_ISLNK(status->st_mode)) {
		ssize_t count = readlink(filesystem_name, linkname, PATH_MAX);
		if (count < 0) {
			warn_path("readlink", filesystem_name);
			return -1;
		}
		linkname[count] = '\0';
		type = '2';
	} else if (S_ISFIFO(status->st_mode))
		type = '6';
	else {
		fprintf(stderr, "pax: unsupported file type: %s\n",
			filesystem_name);
		exit_status = 1;
		return -1;
	}
	if (fill_header(&header, stored_name, status, type,
			type == '1'   ? hardlink
			: type == '2' ? linkname
				      : NULL,
			size) != 0) {
		warn_path("archive name", stored_name);
		return -1;
	}
	if (type == '0') {
		input = fopen(filesystem_name, "rb");
		if (input == NULL) {
			warn_path("open", filesystem_name);
			return -1;
		}
	}
	if (write_all(archive_file, &header, sizeof(header)) != 0 ||
	    (input != NULL &&
	     copy_stream(input, archive_file, (uint64_t)size) != 0) ||
	    pad_output((uint64_t)size) != 0) {
		warn_path("write archive member", stored_name);
		if (input != NULL)
			fclose(input);
		return -1;
	}
	if (input != NULL && fclose(input) != 0)
		warn_path("close", filesystem_name);
	if (hardlink == NULL && S_ISREG(status->st_mode) &&
	    status->st_nlink > 1)
		(void)remember_link(status, stored_name);
	if (verbose)
		fprintf(stderr, "%s\n", stored_name);
	return 0;
}

static int
write_tree(const char *filesystem_name, const char *stored_name)
{
	struct stat status;
	DIR *directory;
	struct dirent *entry;
	if (lstat(filesystem_name, &status) != 0) {
		warn_path("stat", filesystem_name);
		return -1;
	}
	(void)write_member(filesystem_name, stored_name, &status);
	if (!S_ISDIR(status.st_mode))
		return 0;
	directory = opendir(filesystem_name);
	if (directory == NULL) {
		warn_path("open directory", filesystem_name);
		return -1;
	}
	while ((entry = readdir(directory)) != NULL) {
		char *child_fs;
		char *child_stored;
		size_t fs_length;
		size_t stored_length;
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		fs_length = strlen(filesystem_name) + strlen(entry->d_name) + 2;
		stored_length = strlen(stored_name) + strlen(entry->d_name) + 2;
		child_fs = malloc(fs_length);
		child_stored = malloc(stored_length);
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
	if (closedir(directory) != 0)
		warn_path("close directory", filesystem_name);
	return 0;
}

static int
matches_patterns(const char *name, char **patterns, int pattern_count)
{
	int index;
	if (pattern_count == 0)
		return 1;
	for (index = 0; index < pattern_count; index++)
		if (fnmatch(patterns[index], name, 0) == 0)
			return 1;
	return 0;
}

static char *
replace_once(const char *input, const struct substitution *rule, int *matched)
{
	regmatch_t matches[10];
	size_t capacity = strlen(input) + strlen(rule->replacement) + 32;
	size_t length = 0;
	char *output = malloc(capacity);
	const char *cursor = input;
	if (output == NULL)
		return NULL;
	*matched = 0;
	do {
		regmatch_t *whole = &matches[0];
		const char *replacement;
		size_t prefix;
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
		for (replacement = rule->replacement; *replacement != '\0';
		     replacement++) {
			int group = -1;
			if (*replacement == '&')
				group = 0;
			else if (*replacement == '\\' &&
				 replacement[1] >= '0' && replacement[1] <= '9')
				group = *++replacement - '0';
			else if (*replacement == '\\' && replacement[1] != '\0')
				replacement++;
			if (group >= 0 && matches[group].rm_so >= 0) {
				size_t group_length =
				    (size_t)(matches[group].rm_eo -
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
		if (!rule->global || whole->rm_so == whole->rm_eo)
			break;
	} while (*cursor != '\0');
	ENSURE(strlen(cursor));
	strcpy(output + length, cursor);
#undef ENSURE
	return output;
}

static char *
transform_name(const char *name)
{
	struct substitution *rule;
	char *current = strdup(name);
	if (current == NULL)
		return NULL;
	for (rule = substitutions; rule != NULL; rule = rule->next) {
		int matched;
		char *next = replace_once(current, rule, &matched);
		free(current);
		if (next == NULL)
			return NULL;
		current = next;
		if (matched)
			break;
	}
	return current;
}

static int
add_substitution(const char *argument)
{
	char delimiter;
	const char *expression_end;
	const char *replacement_end;
	char *expression;
	struct substitution *rule;
	if (argument[0] == '\0')
		return -1;
	delimiter = argument[0];
	expression_end = strchr(argument + 1, delimiter);
	if (expression_end == NULL)
		return -1;
	replacement_end = strchr(expression_end + 1, delimiter);
	if (replacement_end == NULL)
		return -1;
	expression =
	    strndup(argument + 1, (size_t)(expression_end - argument - 1));
	rule = calloc(1, sizeof(*rule));
	if (expression == NULL || rule == NULL) {
		free(expression);
		free(rule);
		return -1;
	}
	rule->replacement = strndup(
	    expression_end + 1, (size_t)(replacement_end - expression_end - 1));
	if (rule->replacement == NULL ||
	    regcomp(&rule->expression, expression, 0) != 0) {
		free(expression);
		free(rule->replacement);
		free(rule);
		return -1;
	}
	free(expression);
	for (replacement_end++; *replacement_end != '\0'; replacement_end++) {
		if (*replacement_end == 'g')
			rule->global = 1;
		else if (*replacement_end != 'p') {
			regfree(&rule->expression);
			free(rule->replacement);
			free(rule);
			return -1;
		}
	}
	rule->next = substitutions;
	substitutions = rule;
	return 0;
}

static int
remove_existing(const char *path, int directory)
{
	struct stat status;
	if (lstat(path, &status) != 0)
		return errno == ENOENT ? 0 : -1;
	if (directory && S_ISDIR(status.st_mode))
		return 0;
	return S_ISDIR(status.st_mode) ? rmdir(path) : unlink(path);
}

static void
apply_metadata(const char *path, mode_t mode, time_t modification, int symlink)
{
	struct timespec times[2];
	if (!symlink && preserve_mode && chmod(path, mode & 07777) != 0)
		warn_path("chmod", path);
	if (preserve_time) {
		times[0].tv_sec = modification;
		times[0].tv_nsec = 0;
		times[1] = times[0];
		if (utimensat(AT_FDCWD, path, times,
			      symlink ? AT_SYMLINK_NOFOLLOW : 0) != 0)
			warn_path("set time", path);
	}
}

static int
extract_regular(const char *path, uint64_t size, mode_t mode, time_t mtime)
{
	char *template;
	size_t length = strlen(path) + 16;
	int descriptor;
	FILE *output;
	int failed = 0;
	if (make_parents(path) != 0)
		return -1;
	template = malloc(length);
	if (template == NULL)
		return -1;
	(void)snprintf(template, length, "%s.pax.XXXXXX", path);
	descriptor = mkstemp(template);
	if (descriptor < 0) {
		free(template);
		return -1;
	}
	output = fdopen(descriptor, "wb");
	if (output == NULL) {
		close(descriptor);
		unlink(template);
		free(template);
		return -1;
	}
	if (copy_stream(archive_file, output, size) != 0 || fclose(output) != 0)
		failed = 1;
	if (!failed && remove_existing(path, 0) != 0)
		failed = 1;
	if (!failed && rename(template, path) != 0)
		failed = 1;
	if (failed)
		unlink(template);
	else
		apply_metadata(path, mode, mtime, 0);
	free(template);
	return failed ? -1 : 0;
}

static int
consume_padding(uint64_t size)
{
	unsigned char padding[BLOCK_SIZE];
	size_t count = (size_t)((BLOCK_SIZE - size % BLOCK_SIZE) % BLOCK_SIZE);
	return count == 0 || read_all(archive_file, padding, count) == 1 ? 0
									 : -1;
}

static int
read_archive(int extract, char **patterns, int pattern_count)
{
	unsigned char block[BLOCK_SIZE];
	int saw_zero = 0;
	for (;;) {
		struct tar_header *header = (struct tar_header *)block;
		uint64_t size, mode, mtime, stored_checksum;
		char *raw_name;
		char *name;
		int selected;
		int result = read_all(archive_file, block, sizeof(block));
		if (result == 0)
			return saw_zero ? 0 : -1;
		if (result < 0)
			return -1;
		if (zero_block(block)) {
			if (saw_zero)
				return 0;
			saw_zero = 1;
			continue;
		}
		saw_zero = 0;
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
			return -1;
		}
		raw_name = archive_name(header);
		name = raw_name == NULL ? NULL : transform_name(raw_name);
		free(raw_name);
		if (name == NULL)
			return -1;
		selected = matches_patterns(name, patterns, pattern_count);
		if (!extract) {
			if (selected)
				printf("%s\n", name);
			free(name);
			if (skip_payload(size) != 0)
				return -1;
			continue;
		}
		if (!selected) {
			free(name);
			if (skip_payload(size) != 0)
				return -1;
			continue;
		}
		if (!safe_path(name)) {
			fprintf(stderr,
				"pax: refusing unsafe archive path: %s\n",
				name);
			exit_status = 1;
			free(name);
			if (skip_payload(size) != 0)
				return -1;
			continue;
		}
		if (verbose)
			fprintf(stderr, "%s\n", name);
		if (header->type == '0' || header->type == '\0') {
			if (extract_regular(name, size, (mode_t)mode,
					    (time_t)mtime) != 0) {
				warn_path("extract", name);
				free(name);
				return -1;
			}
			if (consume_padding(size) != 0) {
				free(name);
				return -1;
			}
		} else {
			char linkname[sizeof(header->linkname) + 1];
			memcpy(linkname, header->linkname,
			       sizeof(header->linkname));
			linkname[sizeof(header->linkname)] = '\0';
			if (skip_payload(size) != 0) {
				free(name);
				return -1;
			}
			if (header->type == '5') {
				if (make_parents(name) != 0 ||
				    (mkdir(name, (mode_t)mode) != 0 &&
				     errno != EEXIST))
					warn_path("mkdir", name);
				else
					apply_metadata(name, (mode_t)mode,
						       (time_t)mtime, 0);
			} else if (header->type == '2') {
				if (!safe_path(linkname) ||
				    make_parents(name) != 0 ||
				    remove_existing(name, 0) != 0 ||
				    symlink(linkname, name) != 0)
					warn_path("symlink", name);
			} else if (header->type == '1') {
				if (!safe_path(linkname) ||
				    make_parents(name) != 0 ||
				    remove_existing(name, 0) != 0 ||
				    link(linkname, name) != 0)
					warn_path("hard link", name);
			} else if (header->type == '6') {
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

static int
position_for_append(FILE *file)
{
	unsigned char block[BLOCK_SIZE];
	for (;;) {
		struct tar_header *header = (struct tar_header *)block;
		uint64_t size;
		off_t position = ftello(file);
		if (position < 0 || read_all(file, block, sizeof(block)) != 1)
			return -1;
		if (zero_block(block))
			return fseeko(file, position, SEEK_SET);
		if (parse_octal(header->size, sizeof(header->size), &size) !=
			0 ||
		    size > (uint64_t)INT64_MAX)
			return -1;
		size += (BLOCK_SIZE - size % BLOCK_SIZE) % BLOCK_SIZE;
		if (fseeko(file, (off_t)size, SEEK_CUR) != 0)
			return -1;
	}
}

static int
finish_archive(void)
{
	static const unsigned char zeros[BLOCK_SIZE * 2];
	if (write_all(archive_file, zeros, sizeof(zeros)) != 0 ||
	    fflush(archive_file) != 0) {
		fprintf(stderr, "pax: could not finish archive: %s\n",
			strerror(errno));
		return -1;
	}
	return 0;
}

static int
write_operands(char **operands, int count)
{
	int index;
	for (index = 0; index < count; index++) {
		const char *stored = portable_name(operands[index]);
		if (write_tree(operands[index], stored) != 0)
			exit_status = 1;
	}
	return finish_archive();
}

static int
copy_operands(char **operands, int count, const char *destination)
{
	FILE *temporary;
	int saved_directory;
	struct stat status;
	if (count == 0) {
		fprintf(stderr,
			"pax: copy mode requires at least one source\n");
		return -1;
	}
	if (stat(destination, &status) != 0 || !S_ISDIR(status.st_mode)) {
		fprintf(stderr,
			"pax: copy destination is not a directory: %s\n",
			destination);
		return -1;
	}
	temporary = tmpfile();
	if (temporary == NULL)
		return -1;
	archive_file = temporary;
	if (write_operands(operands, count) != 0 ||
	    fseeko(temporary, 0, SEEK_SET) != 0) {
		fclose(temporary);
		return -1;
	}
	saved_directory = open(".", O_RDONLY | O_DIRECTORY);
	if (saved_directory < 0 || chdir(destination) != 0) {
		if (saved_directory >= 0)
			close(saved_directory);
		fclose(temporary);
		return -1;
	}
	(void)read_archive(1, NULL, 0);
	if (fchdir(saved_directory) != 0) {
		fprintf(stderr, "pax: cannot return to original directory\n");
		exit_status = 1;
	}
	close(saved_directory);
	fclose(temporary);
	return exit_status ? -1 : 0;
}

static void
usage(void)
{
	fprintf(stderr,
		"usage: pax [-rv] [-f archive] [-s replacement] [pattern ...]\n"
		"       pax -w [-av] [-f archive] [-x ustar] [file ...]\n"
		"       pax -rw [-v] file ... directory\n");
}

int
main(int argc, char **argv)
{
	int read_mode = 0;
	int write_mode = 0;
	int append_mode = 0;
	int option;
	const char *archive_name_value = NULL;
	const char *format = "ustar";
	const char *open_mode;

	while ((option = getopt(argc, argv, "rwaf:x:s:vp:")) != -1) {
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
			if (add_substitution(optarg) != 0) {
				fprintf(stderr,
					"pax: invalid replacement: %s\n",
					optarg);
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
			return 2;
		}
	}
	if (append_mode && !write_mode) {
		fprintf(stderr, "pax: -a requires -w\n");
		return 2;
	}
	if (read_mode && write_mode) {
		if (archive_name_value != NULL || append_mode ||
		    optind + 1 >= argc) {
			usage();
			return 2;
		}
		return copy_operands(argv + optind, argc - optind - 1,
				     argv[argc - 1]) == 0
			   ? exit_status
			   : 1;
	}
	if (strcmp(format, "ustar") != 0 && strcmp(format, "pax") != 0) {
		fprintf(stderr, "pax: unsupported archive format: %s\n",
			format);
		return 2;
	}
	if (write_mode) {
		if (archive_name_value == NULL ||
		    !strcmp(archive_name_value, "-")) {
			if (append_mode) {
				fprintf(
				    stderr,
				    "pax: cannot append to standard output\n");
				return 2;
			}
			archive_file = stdout;
		} else {
			open_mode = append_mode ? "r+b" : "wb";
			archive_file = fopen(archive_name_value, open_mode);
			if (archive_file == NULL) {
				warn_path("open archive", archive_name_value);
				return 1;
			}
			if (append_mode &&
			    position_for_append(archive_file) != 0) {
				fprintf(stderr,
					"pax: invalid archive for append: %s\n",
					archive_name_value);
				fclose(archive_file);
				return 1;
			}
		}
		if (write_operands(argv + optind, argc - optind) != 0)
			exit_status = 1;
		if (archive_file != stdout && fclose(archive_file) != 0)
			exit_status = 1;
		return exit_status;
	}
	if (archive_name_value == NULL || !strcmp(archive_name_value, "-"))
		archive_file = stdin;
	else {
		archive_file = fopen(archive_name_value, "rb");
		if (archive_file == NULL) {
			warn_path("open archive", archive_name_value);
			return 1;
		}
	}
	if (read_archive(read_mode, argv + optind, argc - optind) != 0) {
		fprintf(stderr, "pax: archive read failed\n");
		exit_status = 1;
	}
	if (archive_file != stdin && fclose(archive_file) != 0)
		exit_status = 1;
	return exit_status;
}
