/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD ar userland command.
 */

#include "userland/base/common/archive.h"
#include "userland/base/common/elf_symbols.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *program = "ar";

struct index_symbol {
	const char *name;
	size_t member;
};

static void usage(void);
static void error_path(const char *path);
static int selected(const struct archive_member *member, int argc, char **argv);
static void verbose_name(const struct archive_member *member);
static int print_member(const struct archive_member *member);
static int extract_member(const struct archive_member *member, int verbose);
static void remove_at(struct archive_file *archive, size_t index);
static void member_release(struct archive_member *member);
static int move_members(struct archive_file *archive, const char *anchor, int after, int argc, char **argv, int verbose);
static size_t position(const struct archive_file *archive, const char *name, int after);
static int read_regular(const char *path, struct archive_member *member);
static int insert(struct archive_file *archive, size_t index, struct archive_member *member);
static int build_symbol_index(struct archive_file *archive);
static void put_be32(unsigned char *destination, uint32_t value);
static size_t stored_member_size(const struct archive_member *member);

/*
 * Runs the ar command.
 */
int
main(
	int argc,
	char **argv)
{
	struct archive_member *member_local;
	struct archive_member member_local1;
	int moved;
	ssize_t found;
	size_t target;
	size_t i_index_for;
	size_t i_index_for1;
	size_t i_index_for2;
	size_t i_index_for3;
	struct archive_file archive = {0};
	char operation;
	const char *position_name;
	const char *options;
	const char *archive_path;
	int after, before, create_silent, update, verbose;
	int modify, failed;
	int argi;

	operation = 0;
	position_name = NULL;
	after = 0;
	before = 0;
	create_silent = 0;
	update = 0;
	verbose = 0;
	modify = 0;
	failed = 0;
	argi = 2;

	/* Validates the command-line arguments. */
	if (argc < 3) {
		usage();

		/* Reports operation failure. */
		return 2;
	}

	/* Process each element required by the operation. */
	options = argv[1][0] == '-' ? argv[1] + 1 : argv[1];
	for (; *options; options++) {
		/* Dispatch the selected command-line option. */
		switch (*options) {
		case 'a':
			after = 1;
			break;
		case 'b':
		case 'i':
			before = 1;
			break;
		case 'c':
			create_silent = 1;
			break;
		case 'u':
			update = 1;
			break;
		case 'v':
			verbose = 1;
			break;
		case 'V':
			puts("zedBSD ar 1");

			/* Reports successful completion. */
			return 0;
		case 's':
			/* Validates the selected operation. */
			if (!operation)
				operation = 's';
			break;
		case 'd':
		case 'm':
		case 'p':
		case 'q':
		case 'r':
		case 't':
		case 'x':
			/* Validates the selected operation. */
			if (operation && operation != 's') {
				usage();

				/* Reports operation failure. */
				return 2;
			}
			operation = *options;
			break;
		default:
			usage();

			/* Reports operation failure. */
			return 2;
		}
	}

	/* Validates the selected operation. */
	if (!operation ||
	    ((after || before) && operation != 'm' && operation != 'r')) {
		usage();

		/* Reports operation failure. */
		return 2;
	}

	/* Handles the after condition. */
	if (after || before) {
		/* Validates the command-line arguments. */
		if (argi >= argc) {
			usage();

			/* Reports operation failure. */
			return 2;
		}
		position_name = archive_basename(argv[argi++]);
	}

	/* Validates the command-line arguments. */
	if (argi >= argc) {
		usage();

		/* Reports operation failure. */
		return 2;
	}
	archive_path = argv[argi++];

	/* Handles the archive read condition. */
	if (archive_read(archive_path, &archive)) {
		/* Handles the reported system error. */
		if (errno != ENOENT || (operation != 'q' && operation != 'r' &&
					operation != 's')) {
			error_path(archive_path);

			/* Reports operation failure. */
			return 1;
		}
		memset(&archive, 0, sizeof(archive));

		/* Handles the create silent condition. */
		if (!create_silent) {
			fprintf(stderr, "%s: creating %s\n", program,
				archive_path);
		}
	}

	/* Validates the selected operation. */
	if (operation == 't' || operation == 'p' || operation == 'x') {
		/* Process each remaining element. */
		for (i_index_for = 0; i_index_for < archive.count; i_index_for++) {
			member_local = &archive.members[i_index_for];

			/* Validates the command-line arguments. */
			if (member_local->special ||
			    !selected(member_local, argc - argi, argv + argi))
				continue;

			/* Validates the selected operation. */
			if (operation == 't') {
				/* Handles the verbose condition. */
				if (verbose)
					verbose_name(member_local);
				else
					puts(member_local->name);
			} else if (operation == 'p') {
				/* Handles the verbose condition. */
				if (verbose)
					printf("\n<%s>\n\n", member_local->name);

				/* Handles the print member condition. */
				if (print_member(member_local)) {
					error_path(member_local->name);
					failed = 1;
				}
			} else if (extract_member(member_local, verbose)) {
				error_path(member_local->name);
				failed = 1;
			}
		}
		archive_free(&archive);

		/* Returns the computed result. */
		return failed;
	}

	/*
	 * Symbol tables become stale after any mutation.  Drop them; the zedBSD
	 * static linker accepts archives without an index and ar -s remains an
	 * explicit, deterministic rewrite operation.
	 */

	/* Process each remaining element. */
	for (i_index_for1 = 0; i_index_for1 < archive.count;) {
		/* Handles the archive condition. */
		if (archive.members[i_index_for1].special)
			remove_at(&archive, i_index_for1);
		else
			i_index_for1++;
	}

	/* Validates the selected operation. */
	if (operation == 'd') {
		/* Process each remaining element. */
		for (i_index_for2 = 0; i_index_for2 < archive.count;) {
			/* Validates the command-line arguments. */
			if (selected(&archive.members[i_index_for2], argc - argi,
				     argv + argi)) {
				/* Handles the verbose condition. */
				if (verbose) {
					printf("d - %s\n",
					       archive.members[i_index_for2].name);
				}
				remove_at(&archive, i_index_for2);
				modify = 1;
			} else {
				i_index_for2++;
			}
		}
	} else if (operation == 'm') {
		moved = move_members(&archive, position_name, after,
			 argc - argi, argv + argi, verbose);

		/* Handles the moved condition. */
		if (moved < 0) {
			error_path(position_name ? position_name
						 : archive_path);
			failed = 1;
		} else if (moved)
			modify = 1;
	} else if (operation == 'q' || operation == 'r') {
		target = position_name
		    ? position(&archive, position_name, after)
		    : archive.count;

		/* Process each remaining command-line operand. */
		for (; argi < argc; argi++) {
			found = -1;

			/* Validates the command-line arguments. */
			if (read_regular(argv[argi], &member_local1)) {
				error_path(argv[argi]);
				failed = 1;
				continue;
			}

			/* Validates the selected operation. */
			if (operation == 'r') {
				/* Process each remaining element. */
				for (i_index_for3 = 0; i_index_for3 < archive.count; i_index_for3++) {
					/* Selects the matching value. */
					if (!strcmp(archive.members[i_index_for3].name,
						    member_local1.name)) {
						found = (ssize_t)i_index_for3;
						break;
					}
				}
			}

			/* Handles the found condition. */
			if (found >= 0 && update &&
			    archive.members[found].mtime >= member_local1.mtime) {
				member_release(&member_local1);
				continue;
			}

			/* Handles the found condition. */
			if (found >= 0) {
				member_release(&archive.members[found]);
				archive.members[found] = member_local1;
			} else if (insert(&archive, target++, &member_local1)) {
				error_path(archive_path);
				member_release(&member_local1);
				failed = 1;
				break;
			}

			/* Handles the verbose condition. */
			if (verbose) {
				printf("%c - %s\n", found >= 0 ? 'r' : 'a',
				       argv[argi]);
			}
			modify = 1;
		}
	} else if (operation == 's') {
		modify = 1;
	}

	/* Handles a failed build symbol index operation. */
	if (modify && build_symbol_index(&archive) != 0) {
		error_path(archive_path);
		failed = 1;
		modify = 0;
	}

	/* Handles a failed archive write atomic operation. */
	if ((modify || operation == 's') &&
	    archive_write_atomic(archive_path, &archive)) {
		error_path(archive_path);
		failed = 1;
	}
	archive_free(&archive);

	/* Returns the computed result. */
	return failed;
}

/* Supports the usage operation. */
static void
usage(
	void)
{
	fprintf(
	    stderr,
	    "usage: ar -[abciuVv] -[dmpqrstx] [position] archive [file ...]\n");
}

/* Supports the error path operation. */
static void
error_path(
	const char *path)
{
	fprintf(stderr, "%s: %s: %s\n", program, path, strerror(errno));
}

/* Supports the selected operation. */
static int
selected(
	const struct archive_member *member,
	int argc,
	char **argv)
{
	int i_index_for;

	/* Validates the command-line arguments. */
	if (!argc)
		return 1;

	/* Process each remaining command-line operand. */
	for (i_index_for = 0; i_index_for < argc; i_index_for++) {
		/* Handles the selected command-line operation. */
		if (!strcmp(member->name, archive_basename(argv[i_index_for])))
			return 1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the verbose name operation. */
static void
verbose_name(
	const struct archive_member *member)
{
	size_t i_index_for;
	char modes[10] = "---------";
	static const unsigned bits[] = {0400, 0200, 0100, 0040, 0020,
					0010, 0004, 0002, 0001};
	static const char chars[] = "rwxrwxrwx";

	/* Process each remaining element. */
	for (i_index_for = 0; i_index_for < 9; i_index_for++) {
		/* Handles the member condition. */
		if (member->mode & bits[i_index_for])
			modes[i_index_for] = chars[i_index_for];
	}
	printf("%s %u/%u %10zu %llu %s\n", modes, member->uid, member->gid,
	       member->size, (unsigned long long)member->mtime, member->name);
}

/* Supports the print member operation. */
static int
print_member(
	const struct archive_member *member)
{
	ssize_t n;
	size_t done;

	/* Process each remaining element. */
	done = 0;
	while (done < member->size) {
		n = write(STDOUT_FILENO, member->data + done,
				  member->size - done);

		/* Handles the reported system error. */
		if (n < 0 && errno == EINTR)
			continue;

		/* Checks the current item count. */
		if (n <= 0)
			return -1;
		done += (size_t)n;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the extract member operation. */
static int
extract_member(
	const struct archive_member *member,
	int verbose)
{
	int saved;
	int saved_local;
	ssize_t count;
	size_t done_for;
	const char *name;
	int fd;

	name = archive_basename(member->name);

	/* Validates the current name. */
	if (!*name || strcmp(name, member->name) || !strcmp(name, ".") ||
	    !strcmp(name, "..")) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, member->mode & 0777);

	/* Checks the file descriptor. */
	if (fd < 0)
		return -1;

	/* Handles a failed fchmod operation. */
	if (fchmod(fd, member->mode & 07777)) {
		saved_local = errno;
		(void)close(fd);
		errno = saved_local;

		/* Reports operation failure. */
		return -1;
	}

	/* Process each remaining element. */
	for (done_for = 0; done_for < member->size;) {
		count = write(fd, member->data + done_for, member->size - done_for);

		/* Handles the reported system error. */
		if (count < 0 && errno == EINTR)
			continue;

		/* Checks the remaining item count. */
		if (count <= 0) {
			saved = count == 0 ? EIO : errno;

			(void)close(fd);
			errno = saved;

			/* Reports operation failure. */
			return -1;
		}
		done_for += (size_t)count;
	}

	/* Handles a failed close operation. */
	if (close(fd) != 0)
		return -1;

	/* Handles the verbose condition. */
	if (verbose)
		printf("x - %s\n", name);

	/* Reports successful completion. */
	return 0;
}

/* Supports the remove at operation. */
static void
remove_at(
	struct archive_file *archive,
	size_t index)
{
	member_release(&archive->members[index]);
	memmove(archive->members + index, archive->members + index + 1,
		(archive->count - index - 1) * sizeof(*archive->members));
	archive->count--;
}

/* Supports the member release operation. */
static void
member_release(
	struct archive_member *member)
{
	free(member->name);
	free(member->data);
	memset(member, 0, sizeof(*member));
}

/* Supports the move members operation. */
static int
move_members(
	struct archive_file *archive,
	const char *anchor,
	int after,
	int argc,
	char **argv,
	int verbose)
{
	size_t index_for;
	size_t index_for1;
	struct archive_member *ordered;
	struct archive_member *moved;
	size_t kept_count;
	size_t moved_count;
	size_t target;

	kept_count = 0;
	moved_count = 0;

	ordered =
	    calloc(archive->count ? archive->count : 1U, sizeof(*ordered));
	moved = calloc(archive->count ? archive->count : 1U, sizeof(*moved));

	/* Handles the ordered availability. */
	if (ordered == NULL || moved == NULL) {
		free(ordered);
		free(moved);

		/* Reports operation failure. */
		return -1;
	}

	/* Process each remaining element. */
	for (index_for = 0; index_for < archive->count; index_for++) {
		/* Validates the command-line arguments. */
		if (selected(&archive->members[index_for], argc, argv)) {
			moved[moved_count++] = archive->members[index_for];

			/* Handles the verbose condition. */
			if (verbose) {
				printf("m - %s\n",
				       archive->members[index_for].name);
			}
		} else {
			ordered[kept_count++] = archive->members[index_for];
		}
	}
	free(archive->members);
	archive->members = ordered;
	archive->count = kept_count;

	/* Handles the anchor availability. */
	if (anchor != NULL) {
		target = position(archive, anchor, after);

		/* Handles the target condition. */
		if (target == archive->count &&
		    (archive->count == 0U ||
		     strcmp(archive->members[archive->count - 1U].name,
			    anchor) != 0)) {
			errno = EINVAL;
			goto fail;
		}
	} else {
		target = archive->count;
	}
	memmove(ordered + target + moved_count, ordered + target,
		(kept_count - target) * sizeof(*ordered));
	memcpy(ordered + target, moved, moved_count * sizeof(*moved));
	archive->count += moved_count;
	free(moved);

	/* Returns the computed result. */
	return moved_count != 0U;

fail:

	/* Process each remaining element. */
	for (index_for1 = 0; index_for1 < moved_count; index_for1++)
		member_release(&moved[index_for1]);
	free(moved);

	/* Reports operation failure. */
	return -1;
}

/* Supports the position operation. */
static size_t
position(
	const struct archive_file *archive,
	const char *name,
	int after)
{
	size_t i_index_for;

	/* Process each remaining element. */
	for (i_index_for = 0; i_index_for < archive->count; i_index_for++) {
		/* Handles the archive condition. */
		if (!archive->members[i_index_for].special &&
		    !strcmp(archive->members[i_index_for].name, name))

			/* Returns the computed result. */
			return i_index_for + (after != 0);
	}

	/* Returns the computed result. */
	return archive->count;
}

/* Supports the read regular operation. */
static int
read_regular(
	const char *path,
	struct archive_member *member)
{
	ssize_t n;
	int saved;
	struct stat st;
	size_t done;
	int fd;

	done = 0;
	fd = open(path, O_RDONLY);

	memset(member, 0, sizeof(*member));

	/* Handles a failed fstat operation. */
	if (fd < 0 || fstat(fd, &st))
		goto fail;

	/* Handles a failed S ISREG operation. */
	if (!S_ISREG(st.st_mode) || st.st_size < 0 ||
	    (uintmax_t)st.st_size > SIZE_MAX) {
		errno = EINVAL;
		goto fail;
	}
	member->name = strdup(archive_basename(path));
	member->data = malloc(st.st_size ? (size_t)st.st_size : 1);

	/* Handles the member condition. */
	if (!member->name || !member->data)
		goto fail;

	/* Process each remaining element. */
	while (done < (size_t)st.st_size) {
		n = read(fd, member->data + done, (size_t)st.st_size - done);

		/* Handles the reported system error. */
		if (n < 0 && errno == EINTR)
			continue;

		/* Checks the current item count. */
		if (n <= 0) {
			/* Checks the current item count. */
			if (!n)
				errno = EIO;
			goto fail;
		}
		done += (size_t)n;
	}
	close(fd);
	member->size = (size_t)st.st_size;
	member->mtime = (uint64_t)st.st_mtime;
	member->uid = st.st_uid;
	member->gid = st.st_gid;
	member->mode = st.st_mode & 07777;

	/* Reports successful completion. */
	return 0;
fail:

saved = errno;

/* Checks the file descriptor. */
if (fd >= 0)
	close(fd);
member_release(member);
errno = saved;

/* Reports operation failure. */
return -1;
}

/* Supports the insert operation. */
static int
insert(
	struct archive_file *archive,
	size_t index,
	struct archive_member *member)
{
	struct archive_member *members;

	/* Checks the current index. */
	if (index > archive->count ||
	    archive->count == SIZE_MAX / sizeof(*members)) {
		errno = EOVERFLOW;

		/* Reports operation failure. */
		return -1;
	}
	members =
	    realloc(archive->members, (archive->count + 1) * sizeof(*members));

	/* Handles the members condition. */
	if (!members)
		return -1;
	archive->members = members;
	memmove(members + index + 1, members + index,
		(archive->count - index) * sizeof(*members));
	members[index] = *member;
	archive->count++;
	memset(member, 0, sizeof(*member));

	/* Reports successful completion. */
	return 0;
}

/* Supports the build symbol index operation. */
static int
build_symbol_index(
	struct archive_file *archive)
{
	struct elf_symbol_record *symbol;
	struct index_symbol *grown;
	size_t name_length;
	size_t stored;
	size_t length;
	unsigned char *names;
	size_t member_for;
	size_t item_for;
	size_t member_for1, item_for2;
	size_t item_for3;
	size_t member_for4;
	size_t member_for5;
	struct elf_symbol_table *tables;
	struct index_symbol *symbols;
	size_t symbol_count;
	size_t names_size;
	size_t data_size;
	size_t member_offset;
	struct archive_member index_member;

	symbols = NULL;
	symbol_count = 0;
	names_size = 0;
	memset(&index_member, 0, sizeof(index_member));

	tables = calloc(archive->count ? archive->count : 1U, sizeof(*tables));

	/* Handles the tables availability. */
	if (tables == NULL)
		return -1;

	/* Process each remaining element. */
	for (member_for = 0; member_for < archive->count; member_for++) {
		/* Handles a failed elf symbols read operation. */
		if (elf_symbols_read(archive->members[member_for].data,
				     archive->members[member_for].size, 0,
				     &tables[member_for]) != 0) {
			/* Handles the reported system error. */
			if (errno == EINVAL || errno == ENOENT)
				continue;
			goto fail;
		}

		/* Process each remaining element. */
		for (item_for = 0; item_for < tables[member_for].count; item_for++) {
			symbol = &tables[member_for].symbols[item_for];

			/* Handles the symbol condition. */
			if ((symbol->binding != 1U && symbol->binding != 2U) ||
			    symbol->section == 0U || symbol->name[0] == '\0')
				continue;
			name_length = strlen(symbol->name) + 1U;

			/* Handles the symbol count condition. */
			if (symbol_count == SIZE_MAX / sizeof(*symbols) ||
			    names_size > SIZE_MAX - name_length)
				goto overflow;
			grown = realloc(symbols,
					(symbol_count + 1U) * sizeof(*symbols));

			/* Handles the grown availability. */
			if (grown == NULL)
				goto fail;
			symbols = grown;
			symbols[symbol_count].name = symbol->name;
			symbols[symbol_count].member = member_for;
			symbol_count++;
			names_size += name_length;
		}
	}

	/* Handles the symbol count condition. */
	if (symbol_count > UINT32_MAX ||
	    symbol_count > (SIZE_MAX - 4U - names_size) / 4U)
		goto overflow;

	data_size = 4U + symbol_count * 4U + names_size;
	index_member.name = strdup("/");
	index_member.data = malloc(data_size ? data_size : 1U);

	/* Handles the name availability. */
	if (index_member.name == NULL || index_member.data == NULL)
		goto fail;

	index_member.size = data_size;
	index_member.mode = 0;
	index_member.special = 1;

	put_be32(index_member.data, (uint32_t)symbol_count);

	member_offset = 8U + 60U + data_size + (data_size & 1U);

	/* Process each remaining element. */
	for (member_for1 = 0, item_for2 = 0; member_for1 < archive->count; member_for1++) {
		stored = stored_member_size(&archive->members[member_for1]);

		/* Handles the stored condition. */
		if (stored == SIZE_MAX || member_offset > UINT32_MAX)
			goto overflow;

		/* Process each remaining element. */
		while (item_for2 < symbol_count && symbols[item_for2].member == member_for1) {
			put_be32(index_member.data + 4U + item_for2++ * 4U,
				 (uint32_t)member_offset);
		}

		/* Handles the member offset condition. */
		if (member_offset > SIZE_MAX - stored)
			goto overflow;
		member_offset += stored;
	}

	names = index_member.data + 4U + symbol_count * 4U;

	/* Process each remaining element. */
	for (item_for3 = 0; item_for3 < symbol_count; item_for3++) {
		length = strlen(symbols[item_for3].name) + 1U;

		(void)memcpy(names, symbols[item_for3].name, length);
		names += length;
	}

	/* Handles a failed insert operation. */
	if (insert(archive, 0, &index_member) != 0)
		goto fail;

	/* Process each remaining element. */
	for (member_for4 = 0; member_for4 + 1U < archive->count; member_for4++)
		elf_symbols_free(&tables[member_for4]);
	free(tables);
	free(symbols);

	/* Reports successful completion. */
	return 0;

overflow:
	errno = EOVERFLOW;
fail:

	/* Process each remaining element. */
	for (member_for5 = 0; member_for5 < archive->count; member_for5++)
		elf_symbols_free(&tables[member_for5]);
	free(tables);
	free(symbols);
	member_release(&index_member);

	/* Reports operation failure. */
	return -1;
}

/* Supports the put be32 operation. */
static void
put_be32(
	unsigned char *destination,
	uint32_t value)
{
	destination[0] = (unsigned char)(value >> 24);
	destination[1] = (unsigned char)(value >> 16);
	destination[2] = (unsigned char)(value >> 8);
	destination[3] = (unsigned char)value;
}

/* Supports the stored member size operation. */
static size_t
stored_member_size(
	const struct archive_member *member)
{
	size_t name_length;
	size_t stored;

	name_length = strlen(member->name);
	stored = member->size;

	/* Handles a failed strchr operation. */
	if (name_length > 15U || strchr(member->name, ' ') != NULL) {
		/* Handles the stored condition. */
		if (stored > SIZE_MAX - name_length)
			return SIZE_MAX;
		stored += name_length;
	}

	/* Handles the stored condition. */
	if (stored > SIZE_MAX - 60U - (stored & 1U))
		return SIZE_MAX;

	/* Returns the computed result. */
	return 60U + stored + (stored & 1U);
}
