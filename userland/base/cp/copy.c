/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Copies files and file hierarchies for cp, and for mv across file systems.
 *
 * A regular file is copied by content into a destination that is opened
 * and truncated, or created with the source's permission bits; with -f a
 * destination that cannot be opened is removed and created again.  With
 * -R a directory is copied entry by entry into a directory that is made
 * owner-writable while it is filled and given its final permissions after,
 * and symbolic links, FIFOs and devices are made anew rather than read.
 * Which symbolic links are followed is the -H, -L or -P policy.
 *
 * With -p the owner, the permission bits and the access and modification
 * times are duplicated after the content; an owner that cannot be given
 * clears the set-user-ID and set-group-ID bits instead of failing.  With
 * -a, names that share one source file share one copy.  An optional report
 * file receives one hex-encoded record for each completed entry, which the
 * installer uses to account for a tree copy.
 */

#include "userland/base/cp/copy.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

/* The deepest directory nesting a tree copy follows. */
#define COPY_DEPTH_MAX 128

/* The size of the buffer the contents of a file are copied through. */
#define COPY_BUFFER_SIZE 65536

/* The first line of a report file. */
#define COPY_REPORT_HEADER "CPCOPY1\n"

/* The last line of a report file, written only after a complete copy. */
#define COPY_REPORT_END "END\n"

static int copy_entry(struct copy_options *options, const char *source, const char *destination, const struct stat *from, unsigned depth);
static int copy_children(struct copy_options *options, const char *source, const char *destination, unsigned depth);
static int copy_child(struct copy_options *options, const char *source, const char *destination, unsigned depth);
static int copy_file(struct copy_options *options, const char *source, const char *destination, const struct stat *from);
static int open_destination(struct copy_options *options, const char *destination, int exists, mode_t mode);
static int copy_contents(int input, int output, int *input_failed);
static int apply_attributes(struct copy_options *options, int descriptor, const struct stat *from, const char *path);
static int copy_directory(struct copy_options *options, const char *source, const char *destination, const struct stat *from, unsigned depth);
static int finish_directory(struct copy_options *options, const char *destination, const struct stat *from, int created, mode_t mask);
static int copy_special(struct copy_options *options, const char *source, const char *destination, const struct stat *from);
static int clear_special_destination(struct copy_options *options, const char *destination, const struct stat *from);
static int apply_special_attributes(struct copy_options *options, const char *destination, const struct stat *from);
static int link_existing(struct copy_options *options, const char *source, const char *destination, const struct stat *from);
static int link_remember(struct copy_options *options, const char *destination, const struct stat *from);
static int link_identity(const struct stat *left, const struct stat *right);
static int admit_tree_destination(const char *source, const char *destination);
static int canonical_path(const char *path, char *resolved);
static int is_below(const char *root, const char *path);
static int join_path(const char *directory, const char *name, char *path, size_t size);
static int write_all(int descriptor, const char *data, size_t length);
static int report(const struct copy_options *options, const char *path);

/*
 * Fills the options with the defaults of a plain copy: operands followed,
 * no recursion, nothing preserved, and no report.
 */
void
copy_options_init(
	struct copy_options *options,
	const char *program)
{
	/* Starts from nothing set. */
	memset(options, 0, sizeof(*options));

	/* Names the program in diagnostics; there is no report yet. */
	options->program = program;
	options->follow = COPY_FOLLOW_OPERANDS;
	options->report_descriptor = -1;
	options->links = NULL;
}

/*
 * Copies one source operand to a destination pathname.
 *
 * Returns 0 when the source was copied or deliberately skipped, and -1
 * after a diagnosed failure.
 */
int
copy_operand(
	struct copy_options *options,
	const char *source,
	const char *destination)
{
	struct stat from;
	int status;
	int admitted;
	int directory;

	/* A failed report stops further copies that could not be reported. */
	if (options->report_failed)
		return -1;

	/* Reads the source; -H and -L follow a symbolic link named here. */
	if (options->follow == COPY_FOLLOW_NONE)
		status = lstat(source, &from);
	else
		status = stat(source, &from);
	if (status != 0) {
		report(options, source);
		return -1;
	}

	/* Refuses to copy a directory into itself or below itself. */
	directory = S_ISDIR(from.st_mode);
	if (directory && options->recursive) {
		admitted = admit_tree_destination(source, destination);
		if (admitted != 0) {
			fprintf(stderr, "%s: cannot copy a directory, '%s', into itself, '%s'\n", options->program, source, destination);
			return -1;
		}
	}

	/* Copies the source and whatever is below it. */
	status = copy_entry(options, source, destination, &from, 0);
	if (status != 0)
		return -1;

	/* Succeeded: the operand was copied or skipped as asked. */
	return 0;
}

/*
 * Opens the report file of --report-file.  The file must not exist yet and
 * must not lie inside any operand, so that the copy never records or
 * overwrites itself.
 */
int
copy_report_open(
	struct copy_options *options,
	const char *path,
	int count,
	char **operands)
{
	char output[PATH_MAX + 1];
	char root[PATH_MAX + 1];
	int index;
	int status;
	int below;
	int descriptor;

	/* Resolves the report path, which may not exist yet. */
	status = canonical_path(path, output);
	if (status != 0) {
		report(options, path);
		return -1;
	}

	/* Refuses a report inside any source or the destination. */
	for (index = 0; index < count; index++) {
		status = canonical_path(operands[index], root);
		if (status != 0) {
			report(options, operands[index]);
			return -1;
		}

		/* A report below an operand would be copied or overwritten. */
		below = is_below(root, output);
		if (below) {
			errno = EINVAL;
			report(options, path);
			return -1;
		}
	}

	/* Creates the report; an existing file is never reused. */
	descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
	if (descriptor < 0) {
		report(options, path);
		return -1;
	}

	/* Writes the header. */
	status = write_all(descriptor, COPY_REPORT_HEADER, strlen(COPY_REPORT_HEADER));
	if (status != 0) {
		report(options, path);
		close(descriptor);
		return -1;
	}

	/* Succeeded: completed entries are recorded from now on. */
	options->report_descriptor = descriptor;
	return 0;
}

/*
 * Records one completed entry in the report: D or F, a tab, the source
 * pathname in hexadecimal, and a newline.  Hexadecimal keeps a newline in
 * a name from looking like the end of a record.
 */
int
copy_report_record(
	struct copy_options *options,
	const char *source,
	int directory)
{
	static const char hex[] = "0123456789abcdef";
	char record[2 * PATH_MAX + 4];
	size_t length;
	size_t index;
	unsigned byte;
	int status;

	/* Without --report-file nothing is recorded. */
	if (options->report_descriptor < 0)
		return 0;
	if (options->report_failed)
		return -1;

	/* Refuses a name too long for one record. */
	length = strlen(source);
	if (length > PATH_MAX) {
		errno = ENAMETOOLONG;
		options->report_failed = 1;
		report(options, source);
		return -1;
	}

	/* The kind of entry, then the encoded name. */
	record[0] = 'F';
	if (directory)
		record[0] = 'D';
	record[1] = '\t';
	for (index = 0; index < length; index++) {
		byte = (unsigned char)source[index];
		record[2 + index * 2] = hex[byte >> 4];
		record[3 + index * 2] = hex[byte & 15];
	}

	/* Ends the record. */
	record[2 + length * 2] = '\n';

	/* Writes the record; a failed report stops the run. */
	status = write_all(options->report_descriptor, record, 3 + length * 2);
	if (status != 0) {
		options->report_failed = 1;
		fprintf(stderr, "%s: completion report: %s\n", options->program, strerror(errno));
		return -1;
	}

	/* Succeeded: one more entry is accounted for. */
	return 0;
}

/*
 * Closes the report, first writing its end line when the whole run
 * succeeded.  Returns -1 when the report could not be completed.
 */
int
copy_report_close(
	struct copy_options *options,
	const char *path,
	int failed)
{
	int status;
	int result;

	/* Nothing to close without --report-file. */
	if (options->report_descriptor < 0)
		return 0;

	/* The end line says that every entry was copied. */
	result = 0;
	if (!failed && !options->report_failed) {
		status = write_all(options->report_descriptor, COPY_REPORT_END, strlen(COPY_REPORT_END));
		if (status != 0) {
			report(options, path);
			result = -1;
		}
	}

	/* A report that cannot be closed may not have reached the disk. */
	status = close(options->report_descriptor);
	options->report_descriptor = -1;
	if (status != 0) {
		report(options, path);
		result = -1;
	}

	/* Reports whether the report is complete. */
	if (result != 0)
		return -1;

	/* Succeeded: the report is complete. */
	return 0;
}

/* Releases the hard-link records of the run. */
void
copy_finish(
	struct copy_options *options)
{
	struct copy_link *entry;
	struct copy_link *next;

	/* Frees each record and its pathname. */
	entry = options->links;
	while (entry != NULL) {
		next = entry->next;
		free(entry->path);
		free(entry);
		entry = next;
	}

	/* No record is left. */
	options->links = NULL;
}

/*
 * Asks a question about a pathname on standard error and reads the answer
 * line from standard input.  Returns 1 for an answer starting with y or Y,
 * the affirmative answers of the POSIX locale, and 0 otherwise.
 */
int
copy_ask(
	const char *program,
	const char *question,
	const char *path)
{
	int byte;
	int first;

	/* Writes the question. */
	fprintf(stderr, "%s: %s '%s'? ", program, question, path);
	fflush(stderr);

	/* Reads the answer line, keeping its first character. */
	first = EOF;
	for (;;) {
		byte = getchar();
		if (byte == EOF || byte == '\n')
			break;
		if (first == EOF)
			first = byte;
	}

	/* An answer starting with y is affirmative. */
	if (first == 'y' || first == 'Y')
		return 1;

	/* Anything else, and no answer at all, declines. */
	return 0;
}

/*
 * Finds the last component of a pathname, ignoring trailing slashes, and
 * stores it in the buffer.  Returns the buffer, or NULL when the component
 * does not fit.
 */
const char *
copy_leaf(
	const char *path,
	char *buffer,
	size_t size)
{
	size_t end;
	size_t start;
	size_t length;

	/* Drops the trailing slashes, keeping one for the root. */
	end = strlen(path);
	while (end > 1 && path[end - 1] == '/')
		end--;

	/* The component starts after the last slash before its end. */
	start = end;
	while (start > 0 && path[start - 1] != '/')
		start--;

	/* The root itself is its own last component. */
	if (start == end && end > 0)
		start = end - 1;

	/* Copies the component. */
	length = end - start;
	if (length + 1 > size)
		return NULL;
	memcpy(buffer, path + start, length);
	buffer[length] = '\0';

	/* Succeeded: the component. */
	return buffer;
}

/*
 * Copies one entry whose status is already known, and records it once it
 * is complete.  Returns 0 when it was copied or skipped and -1 after a
 * diagnosed failure.
 */
static int
copy_entry(
	struct copy_options *options,
	const char *source,
	const char *destination,
	const struct stat *from,
	unsigned depth)
{
	int directory;
	int regular;
	int symbolic;
	int shared;
	int status;

	/* Bounds the nesting a tree copy follows. */
	if (depth >= COPY_DEPTH_MAX) {
		errno = ELOOP;
		report(options, source);
		return -1;
	}

	/*
	 * With -a, a further name of a source file already copied becomes a
	 * link to that copy instead of a second copy.
	 */
	directory = S_ISDIR(from->st_mode);
	regular = S_ISREG(from->st_mode);
	symbolic = S_ISLNK(from->st_mode);
	shared = 0;
	if (options->verbose && (!directory || options->recursive))
		printf("'%s' -> '%s'\n", source, destination);
	if (options->preserve_links && !directory && from->st_nlink > 1)
		shared = 1;
	if (shared) {
		status = link_existing(options, source, destination, from);
		if (status < 0)
			return -1;
		if (status == 1)
			return 0;
		if (status == 2) {
			status = copy_report_record(options, source, 0);
			if (status != 0)
				return -1;
			return 0;
		}
	}

	/* Copies by the kind of file. */
	if (directory) {
		/* A directory is copied only with -R. */
		if (!options->recursive) {
			fprintf(stderr, "%s: -R not specified; omitting directory '%s'\n", options->program, source);
			return -1;
		}

		/* Copies the directory and what is in it. */
		status = copy_directory(options, source, destination, from, depth);
	} else if (regular) {
		status = copy_file(options, source, destination, from);
	} else if (symbolic) {
		/* A link reaches here only when it is not to be followed. */
		status = copy_special(options, source, destination, from);
	} else if (options->recursive) {
		/* -R makes FIFOs and devices anew instead of reading them. */
		status = copy_special(options, source, destination, from);
	} else {
		/* Without -R any other file is read like a regular one. */
		status = copy_file(options, source, destination, from);
	}

	/* A failure has been diagnosed; a skipped entry is not recorded. */
	if (status < 0)
		return -1;
	if (status > 0)
		return 0;

	/* Remembers the copy of a file with other names for -a. */
	if (shared) {
		status = link_remember(options, destination, from);
		if (status != 0)
			return -1;
	}

	/* Records the completed entry. */
	status = copy_report_record(options, source, directory);
	if (status != 0)
		return -1;

	/* Succeeded: the entry and anything below it were copied. */
	return 0;
}

/*
 * Copies every entry of a source directory into the destination
 * directory.  Returns -1 when any entry failed; the others are still
 * copied.
 */
static int
copy_children(
	struct copy_options *options,
	const char *source,
	const char *destination,
	unsigned depth)
{
	char child_source[PATH_MAX + 1];
	char child_destination[PATH_MAX + 1];
	struct dirent *entry;
	DIR *stream;
	int failed;
	int status;
	int dot;
	int dot_dot;

	/* Opens the source directory. */
	stream = opendir(source);
	if (stream == NULL) {
		report(options, source);
		return -1;
	}

	/* Reads each entry, telling a read error from the end. */
	failed = 0;
	for (;;) {
		errno = 0;
		entry = readdir(stream);
		if (entry == NULL) {
			if (errno != 0) {
				report(options, source);
				failed = 1;
			}

			/* The end of the directory ends the loop. */
			break;
		}

		/* Skips the directory itself and its parent. */
		dot = strcmp(entry->d_name, ".");
		dot_dot = strcmp(entry->d_name, "..");
		if (dot == 0 || dot_dot == 0)
			continue;

		/* Names the entry on both sides. */
		status = join_path(source, entry->d_name, child_source, sizeof(child_source));
		if (status != 0) {
			report(options, source);
			failed = 1;
			continue;
		}

		/* The destination of the entry. */
		status = join_path(destination, entry->d_name, child_destination, sizeof(child_destination));
		if (status != 0) {
			report(options, destination);
			failed = 1;
			continue;
		}

		/* Copies it; a failure is remembered and the rest go on. */
		status = copy_child(options, child_source, child_destination, depth + 1);
		if (status != 0)
			failed = 1;

		/* A failed report stops the traversal. */
		if (options->report_failed) {
			failed = 1;
			break;
		}
	}

	/* Closes the directory even after a failure. */
	status = closedir(stream);
	if (status != 0) {
		report(options, source);
		failed = 1;
	}

	/* Reports whether any entry failed. */
	if (failed)
		return -1;

	/* Succeeded: every entry was copied. */
	return 0;
}

/*
 * Copies one entry found while traversing a directory.  Only -L follows a
 * symbolic link here.
 */
static int
copy_child(
	struct copy_options *options,
	const char *source,
	const char *destination,
	unsigned depth)
{
	struct stat from;
	int status;

	/* Reads the entry by the traversal policy. */
	if (options->follow == COPY_FOLLOW_ALL)
		status = stat(source, &from);
	else
		status = lstat(source, &from);
	if (status != 0) {
		report(options, source);
		return -1;
	}

	/* Copies it. */
	status = copy_entry(options, source, destination, &from, depth);
	if (status != 0)
		return -1;

	/* Succeeded: the entry was copied or skipped. */
	return 0;
}

/*
 * Copies the contents of a file to the destination and then the
 * attributes asked for.  Returns 0 when copied, 1 when deliberately
 * skipped, and -1 after a diagnosed failure.
 */
static int
copy_file(
	struct copy_options *options,
	const char *source,
	const char *destination,
	const struct stat *from)
{
	struct stat existing;
	int exists;
	int existing_directory;
	int input;
	int output;
	int status;
	int answer;
	int input_failed;
	int failed;

	/* Learns whether the destination exists, following a link there. */
	exists = 0;
	status = stat(destination, &existing);
	if (status == 0)
		exists = 1;

	/* Decides what to do with an existing destination. */
	if (exists) {
		/* A file copied onto itself would be destroyed. */
		if (existing.st_dev == from->st_dev && existing.st_ino == from->st_ino) {
			fprintf(stderr, "%s: '%s' and '%s' are the same file\n", options->program, source, destination);
			return -1;
		}

		/* -n keeps the destination; --update=none-fail refuses it. */
		if (options->exclusive) {
			if (!options->conflict_fails)
				return 1;
			errno = EEXIST;
			report(options, destination);
			return -1;
		}

		/* A directory is never replaced by a file. */
		existing_directory = S_ISDIR(existing.st_mode);
		if (existing_directory) {
			fprintf(stderr, "%s: cannot overwrite directory '%s' with non-directory\n", options->program, destination);
			return -1;
		}

		/*
		 * -i asks first.  Declining leaves the file, and the status
		 * reports that not every file was copied.
		 */
		if (options->interactive) {
			answer = copy_ask(options->program, "overwrite", destination);
			if (!answer)
				return -1;
		}
	}

	/* Opens the source. */
	input = open(source, O_RDONLY);
	if (input < 0) {
		report(options, source);
		return -1;
	}

	/* Opens or creates the destination with the source's permission bits. */
	output = open_destination(options, destination, exists, from->st_mode & 0777);
	if (output < 0) {
		report(options, destination);
		close(input);
		return -1;
	}

	/* Copies the contents, unless only the attributes are asked for. */
	failed = 0;
	if (!options->attributes_only) {
		status = copy_contents(input, output, &input_failed);
		if (status != 0) {
			if (input_failed)
				report(options, source);
			else
				report(options, destination);
			failed = 1;
		}
	}

	/* Applies the attributes after the last write of the contents. */
	if (!failed) {
		status = apply_attributes(options, output, from, destination);
		if (status != 0)
			failed = 1;
	}

	/* Closes both files; a failed close can lose written data. */
	status = close(output);
	if (status != 0 && !failed) {
		report(options, destination);
		failed = 1;
	}

	/* The source was only read; its close cannot lose anything. */
	close(input);

	/* Reports a failed copy. */
	if (failed)
		return -1;

	/* Succeeded: the file was copied. */
	return 0;
}

/*
 * Opens an existing destination for writing, truncated unless only the
 * attributes are copied, or creates a new one.  With -f a destination
 * that cannot be opened is removed and created again.
 */
static int
open_destination(
	struct copy_options *options,
	const char *destination,
	int exists,
	mode_t mode)
{
	int descriptor;
	int flags;
	int saved;
	int removed;

	/* A new destination is created, never opened through a race. */
	if (!exists) {
		descriptor = open(destination, O_WRONLY | O_CREAT | O_EXCL, mode);
		return descriptor;
	}

	/* An existing destination keeps its inode and its permission bits. */
	flags = O_WRONLY;
	if (!options->attributes_only)
		flags |= O_TRUNC;
	descriptor = open(destination, flags);
	if (descriptor >= 0)
		return descriptor;

	/* Without -f the failure stands. */
	if (!options->force)
		return -1;

	/* -f removes the destination and creates it again. */
	saved = errno;
	removed = unlink(destination);
	if (removed != 0) {
		errno = saved;
		return -1;
	}

	/* Creates the replacement. */
	descriptor = open(destination, O_WRONLY | O_CREAT | O_EXCL, mode);
	return descriptor;
}

/*
 * Copies everything from one descriptor to another.  Stores whether a
 * failure was in reading rather than writing.
 */
static int
copy_contents(
	int input,
	int output,
	int *input_failed)
{
	static char buffer[COPY_BUFFER_SIZE];
	ssize_t count;
	ssize_t written;
	size_t offset;

	/* Copies chunk by chunk until the end of the input. */
	*input_failed = 0;
	for (;;) {
		count = read(input, buffer, sizeof(buffer));
		if (count == 0)
			break;

		/* A read error other than an interruption ends the copy. */
		if (count < 0) {
			if (errno == EINTR)
				continue;
			*input_failed = 1;
			return -1;
		}

		/* Writes the whole chunk, however many writes it takes. */
		offset = 0;
		while (offset < (size_t)count) {
			written = write(output, buffer + offset, (size_t)count - offset);
			if (written < 0) {
				if (errno == EINTR)
					continue;
				return -1;
			}

			/* Goes on after what was written. */
			offset += (size_t)written;
		}
	}

	/* Succeeded: the whole input was copied. */
	return 0;
}

/*
 * Applies the owner, permission bits and times asked for to an open file.
 * The owner goes first because changing it may clear set-ID bits; an
 * owner that cannot be given clears them instead of failing.
 */
static int
apply_attributes(
	struct copy_options *options,
	int descriptor,
	const struct stat *from,
	const char *path)
{
	struct timespec times[2];
	mode_t mode;
	int status;

	/* The permission bits to give, set-ID bits included. */
	mode = from->st_mode & 07777;

	/* Gives the source's owner and group when -p asks for them. */
	if (options->preserve_owner) {
		status = fchown(descriptor, from->st_uid, from->st_gid);
		if (status != 0) {
			/* An owner that cannot be given drops the set-ID bits. */
			if (errno != EPERM) {
				report(options, path);
				return -1;
			}

			/* The copy must not gain set-ID bits for another owner. */
			mode &= ~(mode_t)(S_ISUID | S_ISGID);
		}
	}

	/* Gives the permission bits. */
	if (options->preserve_mode) {
		status = fchmod(descriptor, mode);
		if (status != 0) {
			report(options, path);
			return -1;
		}
	}

	/* Gives the access and modification times. */
	if (options->preserve_times) {
		times[0] = from->st_atim;
		times[1] = from->st_mtim;
		status = futimens(descriptor, times);
		if (status != 0) {
			report(options, path);
			return -1;
		}
	}

	/* Succeeded: every attribute asked for was applied. */
	return 0;
}

/*
 * Copies a directory: makes the destination directory or reuses an
 * existing one, copies every entry into it, and then gives it its
 * permissions and times.
 */
static int
copy_directory(
	struct copy_options *options,
	const char *source,
	const char *destination,
	const struct stat *from,
	unsigned depth)
{
	struct stat target;
	mode_t mask;
	int created;
	int failed;
	int status;
	int existing_directory;

	/* Learns the file creation mask, putting it back at once. */
	mask = umask(0);
	umask(mask);

	/*
	 * Learns whether the destination exists.  An operand may name a link
	 * to a directory; inside the tree a link is not followed.
	 */
	if (depth == 0)
		status = stat(destination, &target);
	else
		status = lstat(destination, &target);

	/* Makes the directory, or checks the existing one. */
	created = 0;
	if (status != 0) {
		/* Anything but a missing destination is an error. */
		if (errno != ENOENT) {
			report(options, destination);
			return -1;
		}

		/* Owner access lets a read-only source directory be filled. */
		umask(0);
		status = mkdir(destination, S_IRWXU);
		umask(mask);
		if (status != 0) {
			report(options, destination);
			return -1;
		}

		/* The directory is new, so it gets the source's bits at the end. */
		created = 1;
	} else {
		/* An existing destination must be a directory other than the source. */
		existing_directory = S_ISDIR(target.st_mode);
		if (!existing_directory) {
			fprintf(stderr, "%s: cannot overwrite non-directory '%s' with directory '%s'\n", options->program, destination, source);
			return -1;
		}

		/* Copying a directory onto itself would lose it. */
		if (target.st_dev == from->st_dev && target.st_ino == from->st_ino) {
			fprintf(stderr, "%s: '%s' and '%s' are the same file\n", options->program, source, destination);
			return -1;
		}
	}

	/* Copies the entries. */
	failed = 0;
	status = copy_children(options, source, destination, depth);
	if (status != 0)
		failed = 1;

	/* Gives the directory its final permissions and times. */
	status = finish_directory(options, destination, from, created, mask);
	if (status != 0)
		failed = 1;

	/* A failed entry is not hidden by a finished directory. */
	if (failed)
		return -1;

	/* Succeeded: the directory and everything in it were copied. */
	return 0;
}

/*
 * Gives a copied directory its final attributes: a directory made by the
 * copy gets the source's permission bits less the creation mask, and -p
 * gives the owner, the exact bits and the times.
 */
static int
finish_directory(
	struct copy_options *options,
	const char *destination,
	const struct stat *from,
	int created,
	mode_t mask)
{
	struct copy_options final_options;
	struct stat attributes;
	int descriptor;
	int status;
	int preserving;
	int failed;

	/* An existing directory is left alone unless attributes are kept. */
	preserving = 0;
	if (options->preserve_mode || options->preserve_owner || options->preserve_times)
		preserving = 1;
	if (!created && !preserving)
		return 0;

	/*
	 * A made directory gets the source's bits less the mask, which is
	 * applied as a kept mode.
	 */
	attributes = *from;
	final_options = *options;
	if (created && !options->preserve_mode) {
		attributes.st_mode = (from->st_mode & ~(mode_t)07777) | (from->st_mode & 0777 & ~mask);
		final_options.preserve_mode = 1;
	}

	/* Opens the directory itself, not a link put in its place. */
	descriptor = open(destination, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	if (descriptor < 0) {
		report(options, destination);
		return -1;
	}

	/* Applies the attributes. */
	failed = 0;
	status = apply_attributes(&final_options, descriptor, &attributes, destination);
	if (status != 0)
		failed = 1;

	/* Closes the directory. */
	status = close(descriptor);
	if (status != 0 && !failed) {
		report(options, destination);
		failed = 1;
	}

	/* Reports a failure. */
	if (failed)
		return -1;

	/* Succeeded: the directory has its final attributes. */
	return 0;
}

/*
 * Makes a symbolic link, FIFO or device anew in the destination instead
 * of reading it.  Returns 0 when made, 1 when skipped, and -1 after a
 * diagnosed failure.
 */
static int
copy_special(
	struct copy_options *options,
	const char *source,
	const char *destination,
	const struct stat *from)
{
	char target[PATH_MAX + 1];
	ssize_t length;
	int status;
	int socket;
	int symbolic;
	int fifo;

	/* The kind of file to make. */
	socket = S_ISSOCK(from->st_mode);
	symbolic = S_ISLNK(from->st_mode);
	fifo = S_ISFIFO(from->st_mode);

	/* A socket cannot be made by a copy. */
	if (socket) {
		errno = EOPNOTSUPP;
		report(options, source);
		return -1;
	}

	/* Reads the whole link before anything at the destination changes. */
	if (symbolic) {
		length = readlink(source, target, sizeof(target));
		if (length < 0) {
			report(options, source);
			return -1;
		}

		/* A link longer than the buffer cannot be copied whole. */
		if ((size_t)length >= sizeof(target)) {
			errno = ENAMETOOLONG;
			report(options, source);
			return -1;
		}

		/* Terminates the text of the link. */
		target[length] = '\0';
	}

	/* Makes room at the destination, or skips it. */
	status = clear_special_destination(options, destination, from);
	if (status != 0)
		return status;

	/* Makes the link or the node; mknod applies the creation mask. */
	if (symbolic)
		status = symlink(target, destination);
	else if (fifo)
		status = mkfifo(destination, from->st_mode & 0777);
	else
		status = mknod(destination, (from->st_mode & S_IFMT) | (from->st_mode & 0777), from->st_rdev);
	if (status != 0) {
		report(options, destination);
		return -1;
	}

	/* Applies the attributes asked for. */
	status = apply_special_attributes(options, destination, from);
	if (status != 0)
		return -1;

	/* Succeeded: the entry was made. */
	return 0;
}

/*
 * Removes whatever is at the destination of a special file, after the
 * checks and questions that apply.  Returns 0 when the name is free, 1
 * to skip, and -1 after a diagnosed failure.
 */
static int
clear_special_destination(
	struct copy_options *options,
	const char *destination,
	const struct stat *from)
{
	struct stat existing;
	int status;
	int answer;
	int existing_directory;

	/* Nothing to do when the name is free. */
	status = lstat(destination, &existing);
	if (status != 0) {
		if (errno == ENOENT)
			return 0;
		report(options, destination);
		return -1;
	}

	/* -n keeps the destination; --update=none-fail refuses it. */
	if (options->exclusive) {
		if (!options->conflict_fails)
			return 1;
		errno = EEXIST;
		report(options, destination);
		return -1;
	}

	/* The source itself is never removed. */
	if (existing.st_dev == from->st_dev && existing.st_ino == from->st_ino) {
		fprintf(stderr, "%s: '%s' is the source itself\n", options->program, destination);
		return -1;
	}

	/* A directory is never replaced by another kind of file. */
	existing_directory = S_ISDIR(existing.st_mode);
	if (existing_directory) {
		fprintf(stderr, "%s: cannot overwrite directory '%s' with non-directory\n", options->program, destination);
		return -1;
	}

	/* -i asks first; declining is reported in the status. */
	if (options->interactive) {
		answer = copy_ask(options->program, "overwrite", destination);
		if (!answer)
			return -1;
	}

	/* Removes the old entry. */
	status = unlink(destination);
	if (status != 0) {
		report(options, destination);
		return -1;
	}

	/* Succeeded: the name is free. */
	return 0;
}

/*
 * Applies the owner, permission bits and times asked for to a link or
 * node without following it.
 */
static int
apply_special_attributes(
	struct copy_options *options,
	const char *destination,
	const struct stat *from)
{
	struct timespec times[2];
	mode_t mode;
	int status;
	int symbolic;

	/* The permission bits to give, set-ID bits included. */
	mode = from->st_mode & 07777;
	symbolic = S_ISLNK(from->st_mode);

	/* Gives the owner; one that cannot be given drops the set-ID bits. */
	if (options->preserve_owner) {
		status = lchown(destination, from->st_uid, from->st_gid);
		if (status != 0) {
			if (errno != EPERM) {
				report(options, destination);
				return -1;
			}

			/* The node must not gain set-ID bits for another owner. */
			mode &= ~(mode_t)(S_ISUID | S_ISGID);
		}
	}

	/* Gives the bits to a node; a link's own bits are not changed. */
	if (options->preserve_mode && !symbolic) {
		status = fchmodat(AT_FDCWD, destination, mode, 0);
		if (status != 0) {
			report(options, destination);
			return -1;
		}
	}

	/* Gives the times to the link or node itself. */
	if (options->preserve_times) {
		times[0] = from->st_atim;
		times[1] = from->st_mtim;
		status = utimensat(AT_FDCWD, destination, times, AT_SYMLINK_NOFOLLOW);
		if (status != 0) {
			report(options, destination);
			return -1;
		}
	}

	/* Succeeded: the attributes were applied. */
	return 0;
}

/*
 * Links a further name of an already copied source file to that copy.
 * Returns 0 when there is no copy yet, 1 when skipped, 2 when linked, and
 * -1 after a diagnosed failure.
 */
static int
link_existing(
	struct copy_options *options,
	const char *source,
	const char *destination,
	const struct stat *from)
{
	struct copy_link *entry;
	struct stat current;
	int status;
	int same;

	/* Finds the copy of the same source file. */
	for (entry = options->links; entry != NULL; entry = entry->next) {
		if (entry->source.st_dev == from->st_dev && entry->source.st_ino == from->st_ino)
			break;
	}

	/* A source file not copied yet is copied normally. */
	if (entry == NULL)
		return 0;

	/* The source must not have changed since the copy. */
	same = link_identity(from, &entry->source);
	if (!same) {
		errno = ESTALE;
		report(options, source);
		return -1;
	}

	/* The copy must still be the file that was made. */
	status = lstat(entry->path, &current);
	if (status != 0) {
		report(options, entry->path);
		return -1;
	}

	/* Compares it with what was made. */
	same = link_identity(&current, &entry->destination);
	if (!same) {
		errno = ESTALE;
		report(options, entry->path);
		return -1;
	}

	/* Deals with an existing destination name. */
	status = lstat(destination, &current);
	if (status == 0) {
		/* -n keeps it; --update=none-fail refuses it. */
		if (options->exclusive) {
			if (!options->conflict_fails)
				return 1;
			errno = EEXIST;
			report(options, destination);
			return -1;
		}

		/* It already is the copy. */
		if (current.st_dev == entry->destination.st_dev && current.st_ino == entry->destination.st_ino)
			return 2;

		/* The source itself is never removed. */
		if (current.st_dev == from->st_dev && current.st_ino == from->st_ino) {
			errno = EINVAL;
			report(options, destination);
			return -1;
		}

		/* Removes the old name. */
		status = unlink(destination);
		if (status != 0) {
			report(options, destination);
			return -1;
		}
	} else if (errno != ENOENT) {
		report(options, destination);
		return -1;
	}

	/* Links the name to the copy. */
	status = link(entry->path, destination);
	if (status != 0) {
		report(options, destination);
		return -1;
	}

	/* Checks that the new name is the copy. */
	status = lstat(destination, &current);
	if (status != 0) {
		report(options, destination);
		return -1;
	}

	/* Compares it with the copy. */
	same = link_identity(&current, &entry->destination);
	if (!same) {
		errno = ESTALE;
		report(options, destination);
		return -1;
	}

	/* Succeeded: the name is a link to the copy. */
	return 2;
}

/* Remembers a completed copy of a source file that has several names. */
static int
link_remember(
	struct copy_options *options,
	const char *destination,
	const struct stat *from)
{
	struct copy_link *entry;
	int status;

	/* Allocates the record. */
	entry = calloc(1, sizeof(*entry));
	if (entry == NULL) {
		report(options, destination);
		return -1;
	}

	/* Keeps the pathname of the copy. */
	entry->path = strdup(destination);
	if (entry->path == NULL) {
		free(entry);
		report(options, destination);
		return -1;
	}

	/* Keeps what the copy is now, to recognize it later. */
	status = lstat(destination, &entry->destination);
	if (status != 0) {
		free(entry->path);
		free(entry);
		report(options, destination);
		return -1;
	}

	/* Adds the record; the run owns it until copy_finish(). */
	entry->source = *from;
	entry->next = options->links;
	options->links = entry;
	return 0;
}

/*
 * Tells whether two statuses describe the same unchanged file.  The link
 * count and the access time are left out: reading and linking change
 * them.
 */
static int
link_identity(
	const struct stat *left,
	const struct stat *right)
{
	/* The same file. */
	if (left->st_dev != right->st_dev)
		return 0;
	if (left->st_ino != right->st_ino)
		return 0;

	/* The same kind, size and owner. */
	if (left->st_mode != right->st_mode)
		return 0;
	if (left->st_size != right->st_size)
		return 0;
	if (left->st_uid != right->st_uid)
		return 0;
	if (left->st_gid != right->st_gid)
		return 0;

	/* Not written since. */
	if (left->st_mtim.tv_sec != right->st_mtim.tv_sec)
		return 0;
	if (left->st_mtim.tv_nsec != right->st_mtim.tv_nsec)
		return 0;

	/* The same unchanged file. */
	return 1;
}

/*
 * Tells whether a directory may be copied to a destination: the
 * destination must not be the directory or lie below it.  Returns 0 when
 * it may.
 */
static int
admit_tree_destination(
	const char *source,
	const char *destination)
{
	char origin[PATH_MAX + 1];
	char target[PATH_MAX + 1];
	int status;
	int below;

	/* Resolves both sides; the destination may not exist yet. */
	status = canonical_path(source, origin);
	if (status != 0)
		return 0;
	status = canonical_path(destination, target);
	if (status != 0)
		return 0;

	/* A destination at or below the source is refused. */
	below = is_below(origin, target);
	if (below) {
		errno = EINVAL;
		return -1;
	}

	/* Succeeded: the destination is outside the source. */
	return 0;
}

/*
 * Resolves a pathname to an absolute one without links, dots or repeated
 * slashes.  A missing last component is allowed when its parent exists.
 */
static int
canonical_path(
	const char *path,
	char *resolved)
{
	char parent[PATH_MAX + 1];
	char canonical[PATH_MAX + 1];
	char *result;
	char *slash;
	const char *leaf;
	size_t length;
	int count;
	int root;

	/* An existing pathname resolves directly. */
	result = realpath(path, resolved);
	if (result != NULL)
		return 0;
	if (errno != ENOENT)
		return -1;

	/* Splits a missing pathname into its parent and its last component. */
	length = strlen(path);
	if (length == 0 || length > PATH_MAX) {
		errno = EINVAL;
		return -1;
	}

	/* Drops the trailing slashes of a copy of the pathname. */
	memcpy(parent, path, length + 1);
	while (length > 1 && parent[length - 1] == '/') {
		length--;
		parent[length] = '\0';
	}

	/* Resolves the parent, which must exist. */
	slash = strrchr(parent, '/');
	leaf = parent;
	if (slash == NULL) {
		result = realpath(".", canonical);
	} else if (slash == parent) {
		leaf = slash + 1;
		result = realpath("/", canonical);
	} else {
		leaf = slash + 1;
		*slash = '\0';
		result = realpath(parent, canonical);
	}

	/* The parent must exist. */
	if (result == NULL)
		return -1;

	/* Joins the parent and the component. */
	root = strcmp(canonical, "/");
	if (root == 0)
		count = snprintf(resolved, PATH_MAX + 1, "/%s", leaf);
	else
		count = snprintf(resolved, PATH_MAX + 1, "%s/%s", canonical, leaf);
	if (count < 0 || count > PATH_MAX) {
		errno = ENAMETOOLONG;
		return -1;
	}

	/* Succeeded: the resolved pathname. */
	return 0;
}

/*
 * Tells whether a resolved pathname is a directory's resolved pathname or
 * lies below it.  /tree2 is not below /tree.
 */
static int
is_below(
	const char *root,
	const char *path)
{
	size_t length;
	int compare;

	/* The path must start with the whole root. */
	length = strlen(root);
	compare = strncmp(root, path, length);
	if (compare != 0)
		return 0;

	/* The root directory holds everything. */
	if (length == 1)
		return 1;

	/* The prefix must end at a component boundary. */
	if (path[length] == '\0')
		return 1;
	if (path[length] == '/')
		return 1;

	/* Only a name that starts the same. */
	return 0;
}

/* Joins a directory and an entry name into a pathname. */
static int
join_path(
	const char *directory,
	const char *name,
	char *path,
	size_t size)
{
	int count;

	/* Writes directory/name. */
	count = snprintf(path, size, "%s/%s", directory, name);
	if (count < 0 || (size_t)count >= size) {
		errno = ENAMETOOLONG;
		return -1;
	}

	/* Succeeded: the joined pathname. */
	return 0;
}

/* Writes a whole buffer, however many writes it takes. */
static int
write_all(
	int descriptor,
	const char *data,
	size_t length)
{
	ssize_t written;
	size_t offset;

	/* Writes until every byte is out. */
	offset = 0;
	while (offset < length) {
		written = write(descriptor, data + offset, length - offset);
		if (written < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}

		/* Goes on after what was written. */
		offset += (size_t)written;
	}

	/* Succeeded: the whole buffer was written. */
	return 0;
}

/* Writes a diagnostic naming a pathname and the current error. */
static int
report(
	const struct copy_options *options,
	const char *path)
{
	int saved;

	/* Names the program, the pathname and the reason. */
	saved = errno;
	fprintf(stderr, "%s: %s: %s\n", options->program, path, strerror(saved));
	errno = saved;
	return -1;
}
