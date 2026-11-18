/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Applies changes to files (POSIX XCU patch).
 *
 *	patch [-blNR] [-c|-e|-n|-u] [-d dir] [-D define] [-i patchfile]
 *	      [-o outfile] [-p num] [-r rejectfile] [file [patchfile]]
 *
 * The patch (standard input, or -i) is read as normal, context, unified
 * or ed differences (parse.c), and each file's hunks are applied to it
 * (apply.c).  The file is the operand, or else the first that exists of
 * the names in the headers and on an Index: line, each with -p num
 * leading components removed (all but the last without -p); a patch that
 * makes a file from nothing names a new one.
 *
 * The patched file replaces the old one, which -b keeps as file.orig, or
 * is written to the end of -o outfile.  Hunks that cannot be applied are
 * written, as they were in the patch, to file.rej or -r rejectfile.  -d
 * changes directory first, -R applies the hunks in reverse, -N skips
 * hunks that are already applied, -l matches lines with their blanks
 * folded, -D define keeps both versions under #ifdef, and -c, -e, -n and
 * -u read the patch as that kind of difference only.
 *
 * patch exits with 0 when every hunk was applied, 1 when some were not,
 * and 2 on trouble.
 */

#include "userland/base/patch/apply.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The status when some hunks were not applied. */
#define PATCH_STATUS_REJECTS 1

/* The status on trouble. */
#define PATCH_STATUS_TROUBLE 2

/*
 * The files the run writes besides the patched ones.
 *
 * output is -o outfile and reject -r rejectfile, open for the whole run
 * when given.  backed_up lists the files already saved by -b, so that a
 * second patch to one file does not save it again.
 */
struct patch_run {
	const struct patch_options *options;
	const struct patch_lines *patch;
	FILE *output;
	FILE *reject;
	char **backed_up;
	long backed_up_count;
	int rejects;
	int trouble;
};

static int read_options(int argc, char **argv, struct patch_options *options);
static int patch_one(struct patch_run *run, const struct patch_file *file);
static char *choose_target(const struct patch_options *options, const struct patch_file *file);
static char *strip_name(const char *name, long strip);
static int is_creation(const struct patch_file *file);
static int write_result(struct patch_run *run, const char *target, const struct patch_lines *original, const struct patch_lines *result, int existed);
static int write_lines(FILE *stream, const struct patch_lines *lines);
static int backup(struct patch_run *run, const char *target, const struct patch_lines *original);
static int write_rejects(struct patch_run *run, const struct patch_file *file, const char *target, const int *outcomes);
static void write_context_hunk(FILE *stream, const struct patch_hunk *hunk);
static void write_context_range(FILE *stream, long start, long count);
static int close_stream(FILE *stream, const char *name);
static void usage(void);

/*
 * Runs patch.
 */
int
main(
	int argc,
	char **argv)
{
	struct patch_options options;
	struct patch_lines patch;
	struct patch_file *files;
	struct patch_run run;
	const char *input;
	long count;
	long index;
	int first;
	int status;

	/*
	 * Reads the options; the file may follow, and after it the patch
	 * file, as historical patch allows, when -i does not name one.
	 */
	first = read_options(argc, argv, &options);
	if (argc - first > 2)
		usage();
	if (first < argc)
		options.file = argv[first];
	if (argc - first == 2) {
		if (options.input != NULL)
			usage();
		options.input = argv[first + 1];
	}

	/* -d changes directory before anything else. */
	if (options.directory != NULL) {
		status = chdir(options.directory);
		if (status != 0) {
			fprintf(stderr, "patch: %s: %s\n", options.directory, strerror(errno));
			return PATCH_STATUS_TROUBLE;
		}
	}

	/* Reads the patch: -i, or standard input. */
	input = options.input;
	if (input == NULL)
		input = "-";
	status = patch_lines_load(input, &patch);
	if (status != 0) {
		fprintf(stderr, "patch: %s: %s\n", input, strerror(errno));
		return PATCH_STATUS_TROUBLE;
	}

	/* Its differences. */
	status = patch_read(&patch, options.format, &files, &count);
	if (status != 0) {
		fprintf(stderr, "patch: malformed patch\n");
		return PATCH_STATUS_TROUBLE;
	}

	/* Nothing to patch is trouble. */
	if (count == 0) {
		fprintf(stderr, "patch: only garbage was found in the patch input\n");
		return PATCH_STATUS_TROUBLE;
	}

	/* Opens -o and -r files, which collect for the whole run. */
	memset(&run, 0, sizeof(run));
	run.options = &options;
	run.patch = &patch;
	if (options.output != NULL) {
		run.output = fopen(options.output, "w");
		if (run.output == NULL) {
			fprintf(stderr, "patch: %s: %s\n", options.output, strerror(errno));
			return PATCH_STATUS_TROUBLE;
		}
	}

	/* -r collects every rejected hunk. */
	if (options.reject != NULL) {
		run.reject = fopen(options.reject, "w");
		if (run.reject == NULL) {
			fprintf(stderr, "patch: %s: %s\n", options.reject, strerror(errno));
			return PATCH_STATUS_TROUBLE;
		}
	}

	/* Patches each file. */
	for (index = 0; index < count; index++) {
		status = patch_one(&run, &files[index]);
		if (status != 0)
			run.trouble = 1;
	}

	/* Closes the collecting files. */
	status = close_stream(run.output, options.output);
	if (status != 0)
		run.trouble = 1;
	status = close_stream(run.reject, options.reject);
	if (status != 0)
		run.trouble = 1;

	/* Trouble, then rejects, decide the status. */
	patch_files_free(files, count);
	patch_lines_free(&patch);
	if (run.trouble)
		return PATCH_STATUS_TROUBLE;
	if (run.rejects)
		return PATCH_STATUS_REJECTS;

	/* Succeeded: every hunk was applied. */
	return 0;
}

/*
 * Reads the options and returns the index of the file operand.  An
 * invalid option ends patch with a usage message.
 */
static int
read_options(
	int argc,
	char **argv,
	struct patch_options *options)
{
	char *end;
	int option;

	/* Nothing asked for yet; no -p. */
	memset(options, 0, sizeof(*options));
	options->strip = -1;

	/* Reads each option. */
	for (;;) {
		option = getopt(argc, argv, "bcd:D:ei:lnNo:p:r:Ru");
		if (option == -1)
			break;

		/* Records what the option asks for. */
		switch (option) {
		case 'b':
			options->backup = 1;
			break;
		case 'c':
			options->format = PATCH_CONTEXT;
			break;
		case 'e':
			options->format = PATCH_ED;
			break;
		case 'n':
			options->format = PATCH_NORMAL;
			break;
		case 'u':
			options->format = PATCH_UNIFIED;
			break;
		case 'd':
			options->directory = optarg;
			break;
		case 'D':
			options->define = optarg;
			break;
		case 'i':
			options->input = optarg;
			break;
		case 'l':
			options->loose = 1;
			break;
		case 'N':
			options->forward = 1;
			break;
		case 'o':
			options->output = optarg;
			break;
		case 'p':
			errno = 0;
			options->strip = strtol(optarg, &end, 10);
			if (end == optarg || *end != '\0' || options->strip < 0 || errno != 0)
				usage();
			break;
		case 'r':
			options->reject = optarg;
			break;
		case 'R':
			options->reverse = 1;
			break;
		default:
			usage();
			break;
		}
	}

	/* Reports where the operand starts. */
	return optind;
}

/*
 * Applies one file's differences: finds the file, applies the hunks,
 * writes the result and the rejects.  Returns -1 on trouble.
 */
static int
patch_one(
	struct patch_run *run,
	const struct patch_file *file)
{
	const struct patch_options *options;
	struct patch_lines original;
	struct patch_lines result;
	struct stat status_of_target;
	char *target;
	int *outcomes;
	long index;
	int status;
	int existed;
	int rejected;
	int skipped;

	/* The file to patch. */
	options = run->options;
	target = choose_target(options, file);
	if (target == NULL) {
		fprintf(stderr, "patch: can't find the file to patch\n");
		run->rejects = 1;
		return 0;
	}

	/* Tells which file is patched. */
	fprintf(stderr, "patching file %s\n", target);

	/* Its lines; a file that does not exist yet is empty. */
	existed = 1;
	status = stat(target, &status_of_target);
	if (status != 0) {
		existed = 0;
		memset(&original, 0, sizeof(original));
	} else {
		status = patch_lines_load(target, &original);
		if (status != 0) {
			fprintf(stderr, "patch: %s: %s\n", target, strerror(errno));
			free(target);
			return -1;
		}
	}

	/* Applies the hunks, or runs the ed script. */
	outcomes = calloc((size_t)file->hunk_count + 1, sizeof(*outcomes));
	if (outcomes == NULL) {
		free(target);
		return -1;
	}

	/* Runs an ed script whole; applies the other kinds hunk by hunk. */
	if (file->format == PATCH_ED)
		status = patch_apply_ed(file, &original, &result);
	else
		status = patch_apply(options, file, &original, &result, outcomes);
	if (status != 0) {
		fprintf(stderr, "patch: %s: cannot apply the differences\n", target);
		free(outcomes);
		free(target);
		return -1;
	}

	/* Reports the hunks that were not applied. */
	rejected = 0;
	skipped = 0;
	for (index = 0; index < file->hunk_count; index++) {
		if (outcomes[index] == PATCH_REJECTED) {
			fprintf(stderr, "Hunk #%ld FAILED.\n", index + 1);
			rejected = 1;
		} else if (outcomes[index] == PATCH_SKIPPED) {
			skipped = 1;
		}
	}

	/* Hunks already applied are ignored under -N. */
	if (skipped)
		fprintf(stderr, "patch: %s: ignoring hunks that are already applied\n", target);
	if (rejected)
		run->rejects = 1;

	/* Writes the result and the rejects. */
	status = write_result(run, target, &original, &result, existed);
	if (status == 0 && rejected)
		status = write_rejects(run, file, target, outcomes);

	/* Frees the file's lines. */
	patch_lines_free(&original);
	patch_lines_free(&result);
	free(outcomes);
	free(target);
	if (status != 0)
		return -1;
	return 0;
}

/*
 * Chooses the file a set of differences applies to: the operand, or the
 * first that exists of the header names and the Index: name after -p,
 * or for a patch that makes a file the new name.  Returns NULL when none
 * is found.
 */
static char *
choose_target(
	const struct patch_options *options,
	const struct patch_file *file)
{
	const char *names[3];
	struct stat status_of_name;
	char *stripped;
	int index;
	int status;
	int compare;
	int creation;

	/* The operand wins. */
	if (options->file != NULL)
		return strdup(options->file);

	/* The header names in order, then the Index: name. */
	names[0] = file->old_name;
	names[1] = file->new_name;
	names[2] = file->index_name;
	for (index = 0; index < 3; index++) {
		if (names[index] == NULL)
			continue;
		stripped = strip_name(names[index], options->strip);
		if (stripped == NULL)
			continue;
		status = stat(stripped, &status_of_name);
		if (status == 0)
			return stripped;
		free(stripped);
	}

	/* A patch that makes a file from nothing names the new file. */
	creation = is_creation(file);
	if (!creation)
		return NULL;
	for (index = 1; index >= 0; index--) {
		if (names[index] == NULL)
			continue;
		compare = strcmp(names[index], "/dev/null");
		if (compare == 0)
			continue;
		stripped = strip_name(names[index], options->strip);
		if (stripped != NULL)
			return stripped;
	}

	/* No file. */
	return NULL;
}

/*
 * Removes num leading components of a name (leading slashes count as
 * one), or all but the last without -p.
 */
static char *
strip_name(
	const char *name,
	long strip)
{
	const char *cursor;
	const char *slash;
	long index;

	/* Without -p only the last component is kept. */
	if (strip < 0) {
		slash = strrchr(name, '/');
		if (slash != NULL)
			name = slash + 1;
		if (*name == '\0')
			return NULL;
		return strdup(name);
	}

	/* Removes the leading components one at a time. */
	cursor = name;
	for (index = 0; index < strip; index++) {
		if (*cursor == '/') {
			while (*cursor == '/')
				cursor++;
			continue;
		}

		/* The component and the slash after it. */
		slash = strchr(cursor, '/');
		if (slash == NULL)
			return NULL;
		cursor = slash + 1;
	}

	/* What is left. */
	if (*cursor == '\0')
		return NULL;
	return strdup(cursor);
}

/* Tells whether a patch makes its file from nothing. */
static int
is_creation(
	const struct patch_file *file)
{
	/* A single hunk with no old lines at the start of an empty file. */
	if (file->hunk_count != 1)
		return 0;
	if (file->format == PATCH_ED)
		return 0;
	if (file->hunks[0].old.count != 0)
		return 0;
	if (file->hunks[0].old_start != 0)
		return 0;
	return 1;
}

/*
 * Writes the patched lines: to -o outfile, or in place of the file,
 * saving the old one first with -b.
 */
static int
write_result(
	struct patch_run *run,
	const char *target,
	const struct patch_lines *original,
	const struct patch_lines *result,
	int existed)
{
	struct stat status_of_target;
	char temporary[PATH_MAX + 1];
	FILE *stream;
	mode_t mode;
	mode_t mask;
	int descriptor;
	int status;
	int count;
	int closed;

	/* -o gets each patched version in turn. */
	if (run->output != NULL) {
		status = write_lines(run->output, result);
		if (status != 0) {
			fprintf(stderr, "patch: %s: %s\n", run->options->output, strerror(errno));
			return -1;
		}

		/* Succeeded: the name. */
		return 0;
	}

	/* -b saves the old file once. */
	if (run->options->backup && existed) {
		status = backup(run, target, original);
		if (status != 0)
			return -1;
	}

	/* The new file keeps the old one's mode, or gets the usual one. */
	mask = umask(0);
	umask(mask);
	mode = 0666 & ~mask;
	if (existed) {
		status = stat(target, &status_of_target);
		if (status == 0)
			mode = status_of_target.st_mode & 07777;
	}

	/* Writes a temporary file beside it and renames it over the file. */
	count = snprintf(temporary, sizeof(temporary), "%s.patch.%ld", target, (long)getpid());
	if (count < 0 || (size_t)count >= sizeof(temporary)) {
		fprintf(stderr, "patch: %s: %s\n", target, strerror(ENAMETOOLONG));
		return -1;
	}

	/* Creates the temporary file only when it does not exist. */
	descriptor = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (descriptor < 0) {
		fprintf(stderr, "patch: %s: %s\n", temporary, strerror(errno));
		return -1;
	}

	/* Writes through a stream. */
	stream = fdopen(descriptor, "w");
	if (stream == NULL) {
		close(descriptor);
		unlink(temporary);
		return -1;
	}

	/* Writes the lines, keeps the mode and replaces the file. */
	status = write_lines(stream, result);
	closed = fclose(stream);
	if (closed != 0)
		status = -1;
	if (status == 0)
		status = chmod(temporary, mode);
	if (status == 0)
		status = rename(temporary, target);
	if (status != 0) {
		fprintf(stderr, "patch: %s: %s\n", target, strerror(errno));
		unlink(temporary);
		return -1;
	}

	/* Succeeded: the file holds the patched lines. */
	return 0;
}

/* Writes lines to a stream, the last one without its newline if it had none. */
static int
write_lines(
	FILE *stream,
	const struct patch_lines *lines)
{
	long index;
	int failed;

	/* Each line and its newline. */
	for (index = 0; index < lines->count; index++) {
		fputs(lines->items[index], stream);
		if (index + 1 < lines->count || !lines->last_unterminated)
			fputc('\n', stream);
	}

	/* Reports a failed write. */
	failed = ferror(stream);
	if (failed)
		return -1;
	return 0;
}

/* Saves the old contents of a file as file.orig, once per file. */
static int
backup(
	struct patch_run *run,
	const char *target,
	const struct patch_lines *original)
{
	char name[PATH_MAX + 1];
	char **grown;
	FILE *stream;
	long index;
	int count;
	int status;
	int compare;
	int closed;

	/* A file already saved is not saved again. */
	for (index = 0; index < run->backed_up_count; index++) {
		compare = strcmp(run->backed_up[index], target);
		if (compare == 0)
			return 0;
	}

	/* Writes file.orig. */
	count = snprintf(name, sizeof(name), "%s.orig", target);
	if (count < 0 || (size_t)count >= sizeof(name))
		return -1;
	stream = fopen(name, "w");
	if (stream == NULL) {
		fprintf(stderr, "patch: %s: %s\n", name, strerror(errno));
		return -1;
	}

	/* Writes the lines. */
	status = write_lines(stream, original);
	closed = fclose(stream);
	if (closed != 0 || status != 0) {
		fprintf(stderr, "patch: %s: %s\n", name, strerror(errno));
		return -1;
	}

	/* Remembers it. */
	grown = realloc(run->backed_up, (size_t)(run->backed_up_count + 1) * sizeof(*grown));
	if (grown == NULL)
		return -1;
	run->backed_up = grown;
	run->backed_up[run->backed_up_count] = strdup(target);
	run->backed_up_count++;
	return 0;
}

/*
 * Writes the rejected hunks, as they were in the patch, to file.rej or
 * to -r rejectfile, after the file headers they need.
 */
static int
write_rejects(
	struct patch_run *run,
	const struct patch_file *file,
	const char *target,
	const int *outcomes)
{
	const struct patch_hunk *hunk;
	const char *old_name;
	const char *new_name;
	char name[PATH_MAX + 1];
	FILE *stream;
	long index;
	long line;
	int count;
	int closed;

	/* The reject file: -r, or file.rej. */
	stream = run->reject;
	if (stream == NULL) {
		count = snprintf(name, sizeof(name), "%s.rej", target);
		if (count < 0 || (size_t)count >= sizeof(name))
			return -1;
		stream = fopen(name, "w");
		if (stream == NULL) {
			fprintf(stderr, "patch: %s: %s\n", name, strerror(errno));
			return -1;
		}
	}

	/* The headers name the files, or the file patched. */
	old_name = file->old_name;
	if (old_name == NULL)
		old_name = target;
	new_name = file->new_name;
	if (new_name == NULL)
		new_name = target;
	if (file->format == PATCH_UNIFIED)
		fprintf(stream, "--- %s\n+++ %s\n", old_name, new_name);
	else
		fprintf(stream, "*** %s\n--- %s\n", old_name, new_name);

	/*
	 * Each rejected hunk: unified and context ones as they were in the
	 * patch, normal ones in the context format.
	 */
	for (index = 0; index < file->hunk_count; index++) {
		if (outcomes[index] != PATCH_REJECTED)
			continue;
		hunk = &file->hunks[index];
		if (file->format == PATCH_NORMAL) {
			write_context_hunk(stream, hunk);
			continue;
		}

		/* Copies the hunk's lines. */
		for (line = hunk->raw_first; line < hunk->raw_end; line++)
			fprintf(stream, "%s\n", run->patch->items[line]);
	}

	/* Closes a file.rej of its own. */
	if (stream == run->reject)
		return 0;
	closed = fclose(stream);
	if (closed != 0)
		return -1;
	return 0;
}

/* Closes a file the run collects into, if open; a failure is reported. */
static int
close_stream(
	FILE *stream,
	const char *name)
{
	int closed;

	/* Nothing to close. */
	if (stream == NULL)
		return 0;

	/* Closes it; a write that failed shows here. */
	closed = fclose(stream);
	if (closed != 0) {
		fprintf(stderr, "patch: %s: %s\n", name, strerror(errno));
		return -1;
	}

	/* Succeeded: the rejects are written. */
	return 0;
}

/*
 * Writes a normal hunk in the context format: "!" marks changed lines,
 * "-" deleted ones and "+" added ones, and an empty side is left out.
 */
static void
write_context_hunk(
	FILE *stream,
	const struct patch_hunk *hunk)
{
	const char *old_mark;
	const char *new_mark;
	long index;

	/* Lines on both sides are changed; on one side deleted or added. */
	old_mark = "- ";
	new_mark = "+ ";
	if (hunk->old.count > 0 && hunk->new.count > 0) {
		old_mark = "! ";
		new_mark = "! ";
	}

	/* The old side. */
	fputs("***************\n*** ", stream);
	write_context_range(stream, hunk->old_start, hunk->old.count);
	fputs(" ****\n", stream);
	for (index = 0; index < hunk->old.count; index++)
		fprintf(stream, "%s%s\n", old_mark, hunk->old.items[index]);

	/* The new side. */
	fputs("--- ", stream);
	write_context_range(stream, hunk->new_start, hunk->new.count);
	fputs(" ----\n", stream);
	for (index = 0; index < hunk->new.count; index++)
		fprintf(stream, "%s%s\n", new_mark, hunk->new.items[index]);
}

/*
 * Writes a context range: the first line, or the first and the last, or
 * for no lines the line before them.
 */
static void
write_context_range(
	FILE *stream,
	long start,
	long count)
{
	/* One number for no line or one line; else both ends. */
	if (count <= 1) {
		fprintf(stream, "%ld", start);
		return;
	}

	/* The first and the last line. */
	fprintf(stream, "%ld,%ld", start, start + count - 1);
}

/* Writes the usage message and exits with the trouble status. */
static void
usage(void)
{
	/* Names the POSIX form. */
	fprintf(stderr,
		"usage: patch [-blNR] [-c|-e|-n|-u] [-d dir] [-D define] [-i patchfile]\n"
		"             [-o outfile] [-p num] [-r rejectfile] [file]\n");
	exit(PATCH_STATUS_TROUBLE);
}
