/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Writes the status of files (the stat command, as GNU's takes it).
 *
 *	stat [-L] [-t] [-c format | --printf=format] file...
 *
 * Without a format each file is described on a few lines.  -c writes the
 * format and a newline for each file, --printf the format alone (with the
 * escapes \n, \t and the others).  The directives are GNU's: %n the name,
 * %N the name quoted (with the target of a link), %s the size, %b and %B
 * the blocks and their size, %o the I/O block, %a and %A the permissions in
 * octal and as ls writes them, %f the raw mode in hexadecimal, %F the type,
 * %u %U %g %G the owner and group (number, name), %h the links, %i the
 * inode, %d and %D the device (decimal, hexadecimal), %x %y %z the access,
 * modification and change times, %X %Y %Z the same in seconds, %w %W the
 * birth time (unknown here), %%.  -t is the terse format.  Links are not
 * followed, unless -L.
 */

#include "userland/base/common/command.h"

#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "include/kern/os-name.h"

/* The codes of the long options that have no letter. */
#define OPTION_PRINTF	256
#define OPTION_HELP	257
#define OPTION_VERSION	258

/* The format of -t. */
#define TERSE_FORMAT	"%n %s %b %f %u %g %D %i %h %t %T %X %Y %Z %W %o"

/*
 * The options written in full.
 *
 * The table is read by the scan of the command line only; the letter a
 * long option shares its code with makes the two forms one case.
 */
static const struct command_long_option stat_long_options[] = {
	{"dereference", COMMAND_VALUE_NONE, 'L'},
	{"format", COMMAND_VALUE_REQUIRED, 'c'},
	{"help", COMMAND_VALUE_NONE, OPTION_HELP},
	{"printf", COMMAND_VALUE_REQUIRED, OPTION_PRINTF},
	{"terse", COMMAND_VALUE_NONE, 't'},
	{"version", COMMAND_VALUE_NONE, OPTION_VERSION},
	{NULL, 0, 0}
};

/* What the command line asks for. */
struct options {
	const char *format;
	int escapes;
	int newline;
	int follow;
};

static int read_options(int argc, char **argv, struct options *options);
static void write_default(const char *path, const struct stat *status);
static void write_format(const struct options *options, const char *path, const struct stat *status);
static void write_escape(const char **cursor);
static void write_directive(char letter, const char *path, const struct stat *status);
static void write_quoted_name(const char *path, const struct stat *status);
static void write_time(const struct timespec *moment);
static void write_owner(uid_t uid);
static void write_group(gid_t gid);
static void mode_text(mode_t mode, char *text);
static const char *type_text(const struct stat *status);
static void usage(void);

/*
 * Runs the stat command.
 */
int
main(
	int argc,
	char **argv)
{
	struct options options;
	struct stat status;
	const char *path;
	int count;
	int index;
	int result;
	int failed;

	/* The options; the files follow argv[0]. */
	memset(&options, 0, sizeof(options));
	count = read_options(argc, argv, &options);
	if (count == 0)
		usage();

	/* Each file. */
	failed = 0;
	for (index = 1; index <= count; index++) {
		/* The status, through a link with -L. */
		path = argv[index];
		if (options.follow)
			result = stat(path, &status);
		else
			result = lstat(path, &status);
		if (result != 0) {
			command_error("stat", path);
			failed = 1;
			continue;
		}

		/* The format, or the description. */
		if (options.format != NULL)
			write_format(&options, path, &status);
		else
			write_default(path, &status);
	}

	/* The output, which must have been written. */
	result = fflush(stdout);
	if (result != 0)
		failed = 1;

	/* A file that could not be described. */
	if (failed)
		return 1;

	/* Succeeded. */
	return 0;
}

/*
 * Reads the options; returns the number of files, which are left in argv
 * from argv[1] on.
 */
static int
read_options(
	int argc,
	char **argv,
	struct options *options)
{
	struct command_options scan;
	int code;

	/* The scan of the command line. */
	memset(&scan, 0, sizeof(scan));
	scan.argc = argc;
	scan.argv = argv;
	scan.program = "stat";
	scan.letters = "Ltc:";
	scan.names = stat_long_options;
	command_options_start(&scan);

	/* Each option in turn. */
	for (;;) {
		code = command_options_next(&scan);
		if (code == COMMAND_OPTION_END)
			break;

		/* The option of its code. */
		switch (code) {
		case 'L':
			options->follow = 1;
			break;
		case 'c':
			options->format = scan.value;
			options->newline = 1;
			options->escapes = 0;
			break;
		case OPTION_PRINTF:
			options->format = scan.value;
			options->newline = 0;
			options->escapes = 1;
			break;
		case 't':
			options->format = TERSE_FORMAT;
			options->newline = 1;
			options->escapes = 0;
			break;
		case OPTION_VERSION:
			printf("stat (" ZEDBSD_OS_NAME ") 1.0\n");
			exit(0);
		default:
			usage();
		}
	}

	/* Succeeded: the files follow argv[0]. */
	return scan.operand_count;
}

/* Describes a file on a few lines, when no format is given. */
static void
write_default(
	const char *path,
	const struct stat *status)
{
	/* The name, the size and the type. */
	printf("  File: %s\n", path);
	printf("  Size: %lld\tBlocks: %lld\tIO Block: %ld\t%s\n",
	       (long long)status->st_size, (long long)status->st_blocks,
	       (long)status->st_blksize, type_text(status));

	/* Where it is, and its links. */
	printf("Device: %llu\tInode: %llu\tLinks: %llu\n",
	       (unsigned long long)status->st_dev,
	       (unsigned long long)status->st_ino,
	       (unsigned long long)status->st_nlink);

	/* Its permissions and owners. */
	printf("Access: (%04o)\tUid: %u\tGid: %u\n",
	       (unsigned)(status->st_mode & 07777), (unsigned)status->st_uid,
	       (unsigned)status->st_gid);

	/* Its times. */
	printf("Access: %lld\nModify: %lld\nChange: %lld\n",
	       (long long)status->st_atime, (long long)status->st_mtime,
	       (long long)status->st_ctime);
}

/* Writes the format of -c, --printf or -t for one file. */
static void
write_format(
	const struct options *options,
	const char *path,
	const struct stat *status)
{
	const char *cursor;

	/* Each character of the format. */
	for (cursor = options->format; *cursor != '\0'; cursor++) {
		/* An escape of --printf. */
		if (*cursor == '\\' && options->escapes && cursor[1] != '\0') {
			cursor++;
			write_escape(&cursor);
			continue;
		}

		/* An ordinary character. */
		if (*cursor != '%' || cursor[1] == '\0') {
			putchar(*cursor);
			continue;
		}

		/* A directive. */
		cursor++;
		write_directive(*cursor, path, status);
	}

	/* -c and -t end each file with a newline. */
	if (options->newline)
		putchar('\n');
}

/*
 * Writes the character of an escape of --printf; the cursor is at the
 * letter after the backslash, and is left at the escape's last character.
 */
static void
write_escape(
	const char **cursor)
{
	static const char letters[] = "abfnrtv\\\"";
	static const char values[] = "\a\b\f\n\r\t\v\\\"";
	const char *found;
	unsigned int value;
	int count;

	/* \NNN: an octal byte. */
	if (**cursor >= '0' && **cursor <= '7') {
		value = 0;
		for (count = 0; count < 3; count++) {
			if (**cursor < '0' || **cursor > '7')
				break;
			value = value * 8U + (unsigned int)(**cursor - '0');
			(*cursor)++;
		}

		/* The byte; the cursor is left at the last digit. */
		(*cursor)--;
		putchar((int)value);
		return;
	}

	/* A letter escape, or the character itself. */
	found = strchr(letters, **cursor);
	if (found != NULL)
		putchar(values[found - letters]);
	else
		putchar(**cursor);
}

/* Writes one directive of a format. */
static void
write_directive(
	char letter,
	const char *path,
	const struct stat *status)
{
	/* A time, copied out: the 32-bit layouts pack struct stat below its time's alignment. */
	struct timespec when;
	char mode[11];

	/* The directive of its letter. */
	switch (letter) {
	case 'n':
		fputs(path, stdout);
		break;
	case 'N':
		write_quoted_name(path, status);
		break;
	case 's':
		printf("%lld", (long long)status->st_size);
		break;
	case 'b':
		printf("%lld", (long long)status->st_blocks);
		break;
	case 'B':
		printf("512");
		break;
	case 'o':
		printf("%ld", (long)status->st_blksize);
		break;
	case 'a':
		printf("%o", (unsigned)(status->st_mode & 07777));
		break;
	case 'A':
		mode_text(status->st_mode, mode);
		fputs(mode, stdout);
		break;
	case 'f':
		printf("%x", (unsigned)status->st_mode);
		break;
	case 'F':
		fputs(type_text(status), stdout);
		break;
	case 'u':
		printf("%lu", (unsigned long)status->st_uid);
		break;
	case 'U':
		write_owner(status->st_uid);
		break;
	case 'g':
		printf("%lu", (unsigned long)status->st_gid);
		break;
	case 'G':
		write_group(status->st_gid);
		break;
	case 'h':
		printf("%llu", (unsigned long long)status->st_nlink);
		break;
	case 'i':
		printf("%llu", (unsigned long long)status->st_ino);
		break;
	case 'd':
		printf("%llu", (unsigned long long)status->st_dev);
		break;
	case 'D':
		printf("%llx", (unsigned long long)status->st_dev);
		break;
	case 't':
	case 'T':
		/* The device numbers of a special file are not split here. */
		printf("0");
		break;
	case 'x':
		when = status->st_atim;
		write_time(&when);
		break;
	case 'y':
		when = status->st_mtim;
		write_time(&when);
		break;
	case 'z':
		when = status->st_ctim;
		write_time(&when);
		break;
	case 'X':
		printf("%lld", (long long)status->st_atime);
		break;
	case 'Y':
		printf("%lld", (long long)status->st_mtime);
		break;
	case 'Z':
		printf("%lld", (long long)status->st_ctime);
		break;
	case 'w':
		/* The birth time is not known. */
		putchar('-');
		break;
	case 'W':
		putchar('0');
		break;
	case '%':
		putchar('%');
		break;
	default:
		/* A directive not known is written as it is. */
		putchar('%');
		putchar(letter);
		break;
	}
}

/* Writes %N: the name in quotes, and the target of a symbolic link. */
static void
write_quoted_name(
	const char *path,
	const struct stat *status)
{
	char target[PATH_MAX + 1];
	ssize_t length;

	/* The name. */
	printf("'%s'", path);
	if ((status->st_mode & S_IFMT) != S_IFLNK)
		return;

	/* The link's target. */
	length = readlink(path, target, sizeof(target) - 1U);
	if (length < 0)
		return;
	target[length] = '\0';
	printf(" -> '%s'", target);
}

/* Writes a time as GNU's stat does: the date, the time to the nanosecond, the zone. */
static void
write_time(
	const struct timespec *moment)
{
	char text[64];
	struct tm fields;
	time_t seconds;

	/* The local date and time, then the nanoseconds and the zone. */
	seconds = moment->tv_sec;
	localtime_r(&seconds, &fields);
	strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &fields);
	printf("%s.%09ld ", text, (long)moment->tv_nsec);
	strftime(text, sizeof(text), "%z", &fields);
	fputs(text, stdout);
}

/* Writes the owner's name, or the number when it has none. */
static void
write_owner(
	uid_t uid)
{
	struct passwd *account;

	/* The account of the number. */
	account = getpwuid(uid);
	if (account == NULL) {
		printf("UNKNOWN");
		return;
	}

	/* Succeeded: its name. */
	fputs(account->pw_name, stdout);
}

/* Writes the group's name, or UNKNOWN when it has none. */
static void
write_group(
	gid_t gid)
{
	struct group *group;

	/* The group of the number. */
	group = getgrgid(gid);
	if (group == NULL) {
		printf("UNKNOWN");
		return;
	}

	/* Succeeded: its name. */
	fputs(group->gr_name, stdout);
}

/* Writes a mode as ls -l does: the type and nine permission letters. */
static void
mode_text(
	mode_t mode,
	char *text)
{
	static const char letters[] = "rwxrwxrwx";
	size_t index;

	/* The type. */
	switch (mode & S_IFMT) {
	case S_IFDIR:
		text[0] = 'd';
		break;
	case S_IFLNK:
		text[0] = 'l';
		break;
	case S_IFCHR:
		text[0] = 'c';
		break;
	case S_IFBLK:
		text[0] = 'b';
		break;
	case S_IFIFO:
		text[0] = 'p';
		break;
	case S_IFSOCK:
		text[0] = 's';
		break;
	default:
		text[0] = '-';
		break;
	}

	/* Each permission, or -. */
	for (index = 0; index < 9U; index++) {
		text[1U + index] = '-';
		if ((mode & (0400U >> index)) != 0)
			text[1U + index] = letters[index];
	}

	/* Succeeded: the text, ended. */
	text[10] = '\0';
}

/* Returns the type of a file as GNU's stat names it. */
static const char *
type_text(
	const struct stat *status)
{
	/* Each type. */
	switch (status->st_mode & S_IFMT) {
	case S_IFREG:
		if (status->st_size == 0)
			return "regular empty file";
		return "regular file";
	case S_IFDIR:
		return "directory";
	case S_IFLNK:
		return "symbolic link";
	case S_IFCHR:
		return "character special file";
	case S_IFBLK:
		return "block special file";
	case S_IFIFO:
		return "fifo";
	case S_IFSOCK:
		return "socket";
	default:
		break;
	}

	/* Anything else. */
	return "unknown";
}

/* Reports the usage and ends stat. */
static void
usage(
	void)
{
	/* The form. */
	fprintf(stderr, "usage: stat [-L] [-t] [-c format | --printf=format] "
		"file...\n");
	exit(1);
}
